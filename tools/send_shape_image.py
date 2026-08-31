#!/usr/bin/env python3
"""Send one image or a test dataset to the CM55 shape recognizer over UART."""

from __future__ import annotations

import argparse
import math
import statistics
import struct
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path


FRAME_MAGIC = b"IMG0"
VERIFIED_MAGIC = b"VHDR"
CHUNK_MAGIC = b"CHNK"
IMAGE_WIDTH = 96
IMAGE_HEIGHT = 96
IMAGE_CHANNELS = 3
IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT * IMAGE_CHANNELS
SUPPORTED_SUFFIXES = {".png", ".jpg", ".jpeg", ".bmp"}
DEFAULT_BAUD = 1000000
DEFAULT_CHUNK_SIZE = 1024
DEFAULT_INTER_CHUNK_DELAY = 0.0
DEFAULT_RETRIES = 2
DEFAULT_ACK_TIMEOUT = 2.0
DEFAULT_CHUNK_RETRIES = 4
DEFAULT_SESSION_OPEN_DELAY = 0.5
DEFAULT_SYNC_TIMEOUT = 4.0
CLASS_LABELS = ("circle", "square", "triangle")


@dataclass
class InferenceRecord:
    image_path: Path
    expected_label: str
    predicted_label: str
    status: str
    time_ms: float
    score: float
    index: int
    scores: tuple[float, ...]
    chunk_resends: int = 0

    @property
    def is_correct(self) -> bool:
        return self.predicted_label.lower() == self.expected_label.lower()

    @property
    def expected_index(self) -> int:
        try:
            return CLASS_LABELS.index(self.expected_label.lower())
        except ValueError:
            return -1

    @property
    def expected_score(self) -> float:
        index = self.expected_index
        if index < 0 or index >= len(self.scores):
            return float("nan")
        return self.scores[index]

    @property
    def score_gap(self) -> float:
        expected_score = self.expected_score
        if not math.isfinite(self.score) or not math.isfinite(expected_score):
            return float("nan")
        return self.score - expected_score


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Send one image or a directory of test images to the CM55 firmware, "
            "then summarize inference timing and recognition results."
        )
    )
    parser.add_argument("port", help="Serial port, for example /dev/ttyUSB0")
    parser.add_argument(
        "input_path",
        help="Path to one image file or a dataset directory with class subdirectories.",
    )
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="UART baud rate")
    parser.add_argument(
        "--timeout",
        type=float,
        default=10.0,
        help="Seconds to wait for one RESULT line",
    )
    parser.add_argument(
        "--ack-timeout",
        type=float,
        default=DEFAULT_ACK_TIMEOUT,
        help="Seconds to wait for one transport ACK/NACK line",
    )
    parser.add_argument(
        "--open-delay",
        type=float,
        default=DEFAULT_SESSION_OPEN_DELAY,
        help="Seconds to wait after opening the serial port",
    )
    parser.add_argument(
        "--sync-timeout",
        type=float,
        default=DEFAULT_SYNC_TIMEOUT,
        help="Seconds to wait for the board banner after opening the serial port",
    )
    parser.add_argument(
        "--protocol",
        choices=("verified", "legacy"),
        default="verified",
        help="UART transport mode to use",
    )
    parser.add_argument(
        "--chunk-size",
        type=int,
        default=DEFAULT_CHUNK_SIZE,
        help="Bytes to send per UART write",
    )
    parser.add_argument(
        "--inter-chunk-delay",
        type=float,
        default=DEFAULT_INTER_CHUNK_DELAY,
        help="Seconds to wait between UART write chunks",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Print only the summary section",
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=DEFAULT_RETRIES,
        help="Additional attempts per image after a timeout",
    )
    parser.add_argument(
        "--chunk-retries",
        type=int,
        default=DEFAULT_CHUNK_RETRIES,
        help="Additional resend attempts per chunk after a NACK or ACK timeout",
    )
    parser.add_argument(
        "--layout",
        choices=("hwc", "chw"),
        default="hwc",
        help="How to flatten RGB pixels before sending them",
    )
    parser.add_argument(
        "--channel-order",
        choices=("rgb", "bgr"),
        default="rgb",
        help="Channel order to send to the device",
    )
    parser.add_argument(
        "--invert",
        action="store_true",
        help="Invert pixel intensities before sending",
    )
    parser.add_argument(
        "--grayscale",
        action="store_true",
        help="Convert the image to grayscale before sending",
    )
    parser.add_argument(
        "--verbose-mistakes",
        type=parse_verbose_mistakes,
        default=0,
        help="Print up to N misclassified images after the summary, or use 'all'",
    )
    return parser.parse_args()


def parse_verbose_mistakes(value: str) -> int | None:
    if value.lower() == "all":
        return None

    try:
        limit = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(
            "verbose-mistakes must be an integer or 'all'"
        ) from exc

    if limit < 0:
        raise argparse.ArgumentTypeError(
            "verbose-mistakes must be non-negative or 'all'"
        )

    return limit


def load_image_bytes(
    image_path: Path,
    layout: str,
    channel_order: str,
    invert: bool,
    grayscale: bool,
) -> bytes:
    try:
        from PIL import Image
    except ImportError as exc:  # pragma: no cover
        raise SystemExit(
            "Pillow is required. Install it with: pip install pillow"
        ) from exc

    image = Image.open(image_path).convert("RGB")
    image = image.resize((IMAGE_WIDTH, IMAGE_HEIGHT))

    if grayscale:
        image = image.convert("L").convert("RGB")

    if invert:
        try:
            from PIL import ImageOps
        except ImportError as exc:  # pragma: no cover
            raise SystemExit(
                "Pillow ImageOps is required. Install it with: pip install pillow"
            ) from exc
        image = ImageOps.invert(image)

    data = image.tobytes()

    if len(data) != IMAGE_BYTES:
        raise SystemExit(
            f"Unexpected resized payload size {len(data)} bytes, expected {IMAGE_BYTES}"
        )

    if channel_order == "bgr":
        pixels = [data[index:index + 3] for index in range(0, len(data), 3)]
        data = b"".join(pixel[::-1] for pixel in pixels)

    if layout == "chw":
        pixels = [data[index:index + 3] for index in range(0, len(data), 3)]
        red = bytes(pixel[0] for pixel in pixels)
        green = bytes(pixel[1] for pixel in pixels)
        blue = bytes(pixel[2] for pixel in pixels)
        data = red + green + blue

    return data


def collect_image_paths(input_path: Path) -> list[Path]:
    if input_path.is_file():
        if input_path.suffix.lower() not in SUPPORTED_SUFFIXES:
            raise SystemExit(f"Unsupported image file: {input_path}")
        return [input_path]

    if not input_path.is_dir():
        raise SystemExit(f"Input path does not exist: {input_path}")

    image_paths = sorted(
        path for path in input_path.rglob("*")
        if path.is_file() and path.suffix.lower() in SUPPORTED_SUFFIXES
    )

    if not image_paths:
        raise SystemExit(f"No supported image files found under: {input_path}")

    return image_paths


def parse_result_line(line: str) -> dict[str, str]:
    if not line.startswith("RESULT "):
        raise ValueError(f"Unexpected device response: {line}")

    result: dict[str, str] = {}
    for token in line.split()[1:]:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        result[key] = value

    required_keys = {"status", "index", "label", "time_us", "scores_q"}
    missing = required_keys.difference(result)
    if missing:
        raise ValueError(f"Missing fields in RESULT line: {sorted(missing)}")

    return result


def parse_key_value_tokens(line: str) -> dict[str, str]:
    result: dict[str, str] = {}

    for token in line.split()[2:]:
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        result[key] = value

    return result


def is_background_device_line(line: str) -> bool:
    prefixes = (
        "CM55 shape recognizer is ready.",
        "Verified protocol:",
        "Legacy protocol:",
        "Self-test command:",
        "Labels:",
        "Unknown threshold:",
        "Waiting for image frames...",
        "Embedded self-test:",
        "SELFTEST ",
        "TRACE ",
    )
    return line.startswith(prefixes)


def read_matching_line(ser, timeout: float, prefixes: tuple[str, ...]) -> str:
    deadline = time.monotonic() + timeout

    while time.monotonic() < deadline:
        line = ser.readline()
        if not line:
            continue

        decoded = line.decode("utf-8", errors="replace").strip()
        if not decoded:
            continue

        if decoded.startswith(prefixes):
            return decoded

        if is_background_device_line(decoded):
            continue

    expected = ", ".join(prefixes)
    raise TimeoutError(f"Timed out waiting for device line: {expected}")


def sync_device_after_open(ser, sync_timeout: float) -> None:
    if sync_timeout <= 0:
        return

    deadline = time.monotonic() + sync_timeout
    saw_activity = False
    last_activity = time.monotonic()

    while time.monotonic() < deadline:
        line = ser.readline()
        if not line:
            if not saw_activity:
                return
            if time.monotonic() - last_activity >= 0.3:
                return
            continue

        decoded = line.decode("utf-8", errors="replace").strip()
        if not decoded:
            continue

        saw_activity = True
        last_activity = time.monotonic()

        if decoded.startswith("Waiting for image frames..."):
            return


def read_result_line(ser, timeout: float) -> dict[str, str]:
    return parse_result_line(read_matching_line(ser, timeout, ("RESULT ",)))


def infer_expected_label(image_path: Path) -> str:
    return image_path.parent.name


def parse_scores(raw_scores: str) -> tuple[float, ...]:
    """Parse STM32N6 output logits, transmitted as the original int8 values."""
    return tuple(float(value) for value in raw_scores.split(","))


def format_score_vector(scores: tuple[float, ...]) -> str:
    pairs: list[str] = []

    for index, score in enumerate(scores):
        label = CLASS_LABELS[index] if index < len(CLASS_LABELS) else f"class_{index}"
        pairs.append(f"{label}={score:.6f}")

    return ", ".join(pairs)


def format_stats(values: list[float]) -> str:
    if not values:
        return "avg=n/a min=n/a max=n/a median=n/a"

    return (
        f"avg={statistics.fmean(values):.3f} ms "
        f"min={min(values):.3f} ms "
        f"max={max(values):.3f} ms "
        f"median={statistics.median(values):.3f} ms"
    )


def print_summary(records: list[InferenceRecord]) -> None:
    total = len(records)
    correct = sum(record.is_correct for record in records)
    timeout_count = sum(record.status == "timeout" for record in records)
    total_resends = sum(record.chunk_resends for record in records)
    all_times = [record.time_ms for record in records if math.isfinite(record.time_ms)]

    print("\nSummary")
    print(
        f"overall: images={total} correct={correct} timeouts={timeout_count} "
        f"chunk_resends={total_resends} accuracy={100.0 * correct / total:.2f}% "
        f"{format_stats(all_times)}"
    )

    expected_labels = sorted({record.expected_label for record in records})
    for label in expected_labels:
        label_records = [record for record in records if record.expected_label == label]
        label_times = [
            record.time_ms for record in label_records if math.isfinite(record.time_ms)
        ]
        label_correct = sum(record.is_correct for record in label_records)
        label_timeouts = sum(record.status == "timeout" for record in label_records)
        print(
            f"{label}: images={len(label_records)} correct={label_correct} "
            f"timeouts={label_timeouts} "
            f"accuracy={100.0 * label_correct / len(label_records):.2f}% "
            f"{format_stats(label_times)}"
        )

    print("confusion:")
    for label in expected_labels:
        label_records = [record for record in records if record.expected_label == label]
        counts: dict[str, int] = {}

        for record in label_records:
            counts[record.predicted_label] = counts.get(record.predicted_label, 0) + 1

        breakdown = ", ".join(
            f"{predicted}={counts[predicted]}" for predicted in sorted(counts)
        )
        print(f"{label} -> {breakdown}")


def print_mistake_details(records: list[InferenceRecord], limit: int | None) -> None:
    if limit == 0:
        return

    mistakes = [
        record
        for record in records
        if record.status != "timeout" and not record.is_correct
    ]

    if not mistakes:
        print("mistakes: none")
        return

    print("mistakes:")
    selected_mistakes = mistakes if limit is None else mistakes[:limit]
    for record in selected_mistakes:
        print(
            f"{record.image_path}: expected={record.expected_label} "
            f"predicted={record.predicted_label}(index={record.index}) "
            f"predicted_score={record.score:.6f} "
            f"expected_score={record.expected_score:.6f} "
            f"score_gap={record.score_gap:.6f} "
            f"scores=[{format_score_vector(record.scores)}]"
        )


def send_frame(
    ser,
    payload: bytes,
    chunk_size: int,
    inter_chunk_delay: float,
) -> None:
    data = FRAME_MAGIC + payload

    for start in range(0, len(data), chunk_size):
        ser.write(data[start:start + chunk_size])
        ser.flush()
        if inter_chunk_delay > 0:
            time.sleep(inter_chunk_delay)


class TransferError(RuntimeError):
    pass


def send_verified_frame(
    ser,
    payload: bytes,
    chunk_size: int,
    inter_chunk_delay: float,
    ack_timeout: float,
    chunk_retries: int,
) -> int:
    if chunk_size <= 0 or chunk_size > 1024:
        raise ValueError("Verified transport chunk size must be in the range 1..1024")

    chunk_count = math.ceil(len(payload) / chunk_size)
    image_crc32 = zlib.crc32(payload) & 0xFFFFFFFF
    header = VERIFIED_MAGIC + struct.pack(
        "<IHHI",
        len(payload),
        chunk_size,
        chunk_count,
        image_crc32,
    )
    chunk_packets: list[bytes] = []
    for chunk_index, offset in enumerate(range(0, len(payload), chunk_size)):
        chunk = payload[offset:offset + chunk_size]
        chunk_crc32 = zlib.crc32(chunk) & 0xFFFFFFFF
        chunk_packets.append(
            CHUNK_MAGIC
            + struct.pack("<HHI", chunk_index, len(chunk), chunk_crc32)
            + chunk
        )

    total_resends = 0

    # Keep one chunk queued ahead. This matches the USB CDC buffering behavior
    # of the board far more reliably than a strict stop-and-wait exchange.
    first_packet = FRAME_MAGIC + header + chunk_packets[0]
    if chunk_count > 1:
        first_packet += chunk_packets[1]

    ser.write(first_packet)
    ser.flush()
    if inter_chunk_delay > 0:
        time.sleep(inter_chunk_delay)

    next_chunk_to_send = 2 if chunk_count > 1 else 1
    next_chunk_to_ack = 0
    drain_sent = False

    while next_chunk_to_ack < chunk_count:
        response_line = read_matching_line(
            ser,
            ack_timeout,
            ("ACK START", "NACK START", "ACK CHUNK", "NACK CHUNK", "ACK IMAGE", "NACK IMAGE"),
        )

        if response_line.startswith("NACK START"):
            raise TransferError(response_line)

        if response_line.startswith("ACK START"):
            continue

        if response_line.startswith("NACK IMAGE"):
            raise TransferError(response_line)

        if response_line.startswith("ACK IMAGE"):
            break

        response = parse_key_value_tokens(response_line)
        response_index = int(response.get("index", "-1"))
        if response_index != next_chunk_to_ack:
            raise TransferError(
                f"Unexpected chunk response for {response_index}, expected {next_chunk_to_ack}: "
                f"{response_line}"
            )

        if response_line.startswith("NACK CHUNK"):
            total_resends += 1
            raise TransferError(response_line)

        next_chunk_to_ack += 1

        if next_chunk_to_send < chunk_count:
            ser.write(chunk_packets[next_chunk_to_send])
            ser.flush()
            if inter_chunk_delay > 0:
                time.sleep(inter_chunk_delay)
            next_chunk_to_send += 1
        elif not drain_sent:
            # One trailing byte encourages the CDC stack to release the final
            # packet so the device can finish image CRC validation.
            ser.write(b"\n")
            ser.flush()
            drain_sent = True

    return total_resends


def run_inference_session(
    port: str,
    baud: int,
    image_paths: list[Path],
    timeout: float,
    ack_timeout: float,
    open_delay: float,
    sync_timeout: float,
    protocol: str,
    chunk_size: int,
    inter_chunk_delay: float,
    retries: int,
    chunk_retries: int,
    quiet: bool,
    layout: str,
    channel_order: str,
    invert: bool,
    grayscale: bool,
) -> list[InferenceRecord]:
    try:
        import serial
    except ImportError as exc:  # pragma: no cover
        raise SystemExit(
            "pyserial is required. Install it with: pip install pyserial"
        ) from exc

    def open_serial_port():
        ser = serial.Serial(port=port, baudrate=baud, timeout=0.2)
        if open_delay > 0:
            time.sleep(open_delay)
        ser.reset_input_buffer()
        sync_device_after_open(ser, sync_timeout)
        return ser

    records: list[InferenceRecord] = []
    ser = open_serial_port()

    try:
        for image_path in image_paths:
            payload = load_image_bytes(
                image_path=image_path,
                layout=layout,
                channel_order=channel_order,
                invert=invert,
                grayscale=grayscale,
            )
            expected_label = infer_expected_label(image_path)
            record: InferenceRecord | None = None

            for attempt in range(retries + 1):
                try:
                    chunk_resends = 0
                    if protocol == "verified":
                        chunk_resends = send_verified_frame(
                            ser=ser,
                            payload=payload,
                            chunk_size=chunk_size,
                            inter_chunk_delay=inter_chunk_delay,
                            ack_timeout=ack_timeout,
                            chunk_retries=chunk_retries,
                        )
                    else:
                        send_frame(ser, payload, chunk_size, inter_chunk_delay)

                    result = read_result_line(ser, timeout)
                    record = InferenceRecord(
                        image_path=image_path,
                        expected_label=expected_label,
                        predicted_label=result["label"],
                        status=result["status"],
                        time_ms=float(result["time_us"]) / 1000.0,
                        score=max(parse_scores(result["scores_q"])),
                        index=int(result["index"]),
                        scores=parse_scores(result["scores_q"]),
                        chunk_resends=chunk_resends,
                    )
                    break
                except (TimeoutError, TransferError):
                    try:
                        ser.reset_input_buffer()
                    except serial.SerialException:
                        ser.close()
                        ser = open_serial_port()

                    if attempt == retries:
                        record = InferenceRecord(
                            image_path=image_path,
                            expected_label=expected_label,
                            predicted_label="timeout",
                            status="timeout",
                            time_ms=float("nan"),
                            score=float("nan"),
                            index=-1,
                            scores=tuple(),
                            chunk_resends=0,
                        )
                    else:
                        time.sleep(0.1)
                except serial.SerialException:
                    try:
                        ser.close()
                    except serial.SerialException:
                        pass

                    if attempt == retries:
                        raise

                    time.sleep(0.5)
                    ser = open_serial_port()

            if record is None:  # pragma: no cover
                raise SystemExit(f"Internal error while processing {image_path}")

            records.append(record)

            if not quiet:
                if record.status == "timeout":
                    print(
                        f"{record.image_path}: expected={record.expected_label} "
                        f"predicted=Unknown status=timeout after {retries + 1} attempts"
                    )
                else:
                    verdict = "OK" if record.is_correct else "MISS"
                    resend_suffix = (
                        f" chunk_resends={record.chunk_resends}"
                        if protocol == "verified"
                        else ""
                    )
                    print(
                        f"{record.image_path}: expected={record.expected_label} "
                        f"predicted={record.predicted_label}(index={record.index}) "
                        f"status={record.status} time_ms={record.time_ms:.3f} "
                        f"score={record.score:.6f} "
                        f"scores=[{format_score_vector(record.scores)}]{resend_suffix} {verdict}"
                    )
    finally:
        ser.close()

    return records


def main() -> int:
    args = parse_args()
    input_path = Path(args.input_path).expanduser().resolve()
    image_paths = collect_image_paths(input_path)
    records = run_inference_session(
        port=args.port,
        baud=args.baud,
        image_paths=image_paths,
        timeout=args.timeout,
        ack_timeout=args.ack_timeout,
        open_delay=args.open_delay,
        sync_timeout=args.sync_timeout,
        protocol=args.protocol,
        chunk_size=args.chunk_size,
        inter_chunk_delay=args.inter_chunk_delay,
        retries=args.retries,
        chunk_retries=args.chunk_retries,
        quiet=args.quiet,
        layout=args.layout,
        channel_order=args.channel_order,
        invert=args.invert,
        grayscale=args.grayscale,
    )
    print_summary(records)
    print_mistake_details(records, args.verbose_mistakes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
