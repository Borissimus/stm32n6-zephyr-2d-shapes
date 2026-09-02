#!/usr/bin/env python3
"""Reliably send Shapes2D RGB frames through the COBS UART protocol."""

from __future__ import annotations

import argparse
import math
import secrets
import statistics
import struct
import sys
import time
import zlib
from dataclasses import dataclass
from pathlib import Path


PROTOCOL_VERSION = 1
PACKET_START = 1
PACKET_DATA = 2
PACKET_ABORT = 3
PACKET_ACK_START = 0x81
PACKET_ACK_DATA = 0x82
PACKET_NACK = 0x83
PACKET_RESULT = 0x84

IMAGE_WIDTH = 96
IMAGE_HEIGHT = 96
IMAGE_CHANNELS = 3
IMAGE_BYTES = IMAGE_WIDTH * IMAGE_HEIGHT * IMAGE_CHANNELS
CHUNK_BYTES = 1024
CHUNK_COUNT = IMAGE_BYTES // CHUNK_BYTES
CLASS_LABELS = ("circle", "square", "triangle")
SUPPORTED_SUFFIXES = {".png", ".jpg", ".jpeg", ".bmp"}


@dataclass
class Record:
    path: Path
    expected: str
    predicted: str
    inference_ms: float
    preprocess_ms: float
    round_trip_ms: float
    frame_retries: int
    packet_retries: int

    @property
    def correct(self) -> bool:
        return self.expected.lower() == self.predicted.lower()


class ProtocolError(RuntimeError):
    pass


def cobs_encode(payload: bytes) -> bytes:
    output = bytearray(b"\x00")
    code_index = 0
    code = 1
    for value in payload:
        if value == 0:
            output[code_index] = code
            code_index = len(output)
            output.append(0)
            code = 1
        else:
            output.append(value)
            code += 1
            if code == 0xFF:
                output[code_index] = code
                code_index = len(output)
                output.append(0)
                code = 1
    output[code_index] = code
    return bytes(output)


def cobs_decode(encoded: bytes) -> bytes:
    output = bytearray()
    index = 0
    while index < len(encoded):
        code = encoded[index]
        index += 1
        if code == 0 or index + code - 1 > len(encoded):
            raise ProtocolError("invalid COBS frame")
        output.extend(encoded[index:index + code - 1])
        index += code - 1
        if code != 0xFF and index < len(encoded):
            output.append(0)
    return bytes(output)


def packet_start(session_id: int, image_crc: int) -> bytes:
    return struct.pack(
        "<BBIII", PROTOCOL_VERSION, PACKET_START, session_id, IMAGE_BYTES, image_crc
    )


def packet_data(session_id: int, sequence: int, payload: bytes) -> bytes:
    if len(payload) != CHUNK_BYTES:
        raise ValueError("all Shapes2D chunks must be exactly 1024 bytes")
    return struct.pack("<BBIH", PROTOCOL_VERSION, PACKET_DATA, session_id, sequence) + payload


def packet_abort(session_id: int) -> bytes:
    return struct.pack("<BBI", PROTOCOL_VERSION, PACKET_ABORT, session_id)


def send_packet(ser, packet: bytes) -> None:
    ser.write(cobs_encode(packet) + b"\x00")
    ser.flush()


def read_packet(ser, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    encoded = bytearray()
    while time.monotonic() < deadline:
        data = ser.read(1)
        if not data:
            continue
        if data == b"\x00":
            if not encoded:
                continue
            try:
                return cobs_decode(bytes(encoded))
            finally:
                encoded.clear()
        if len(encoded) >= 1100:
            encoded.clear()
            continue
        encoded.extend(data)
    raise TimeoutError("timed out waiting for a COBS response")


def read_response(ser, session_id: int, timeout: float) -> bytes:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            packet = read_packet(ser, max(0.01, deadline - time.monotonic()))
        except ProtocolError:
            continue
        if len(packet) >= 6 and packet[0] == PROTOCOL_VERSION:
            response_session = struct.unpack_from("<I", packet, 2)[0]
            if response_session == session_id:
                return packet
    raise TimeoutError("timed out waiting for matching response")


def response_kind(packet: bytes) -> int:
    if len(packet) < 2 or packet[0] != PROTOCOL_VERSION:
        raise ProtocolError("invalid response version")
    return packet[1]


def parse_nack(packet: bytes) -> tuple[int, int]:
    if len(packet) != 9:
        raise ProtocolError("invalid NACK length")
    _, _, _, reason, expected = struct.unpack("<BBIBH", packet)
    return reason, expected


def parse_result(packet: bytes) -> tuple[str, float, float, tuple[int, int, int]]:
    if len(packet) != 19:
        raise ProtocolError("invalid RESULT length")
    _, _, _, status, index, inference_us, preprocess_us, score0, score1, score2 = struct.unpack(
        "<BBIBBIIbbb", packet
    )
    if status != 0 or index >= len(CLASS_LABELS):
        raise ProtocolError(f"board inference failed: status={status} index={index}")
    return (
        CLASS_LABELS[index],
        inference_us / 1000.0,
        preprocess_us / 1000.0,
        (score0, score1, score2),
    )


def load_image(path: Path) -> bytes:
    try:
        from PIL import Image
    except ImportError as exc:
        raise SystemExit("Pillow is required: python -m pip install pillow") from exc

    image = Image.open(path).convert("RGB").resize((IMAGE_WIDTH, IMAGE_HEIGHT))
    payload = image.tobytes()
    if len(payload) != IMAGE_BYTES:
        raise RuntimeError(f"unexpected payload length: {len(payload)}")
    return payload


def collect_images(input_path: Path) -> list[Path]:
    if input_path.is_file():
        return [input_path]
    images = sorted(
        path for path in input_path.rglob("*")
        if path.is_file() and path.suffix.lower() in SUPPORTED_SUFFIXES
    )
    if not images:
        raise SystemExit(f"no image files found under {input_path}")
    return images


def wait_for_start_ack(ser, session_id: int, image_crc: int, retries: int) -> int:
    for attempt in range(retries + 1):
        send_packet(ser, packet_start(session_id, image_crc))
        try:
            response = read_response(ser, session_id, 1.5)
        except TimeoutError:
            continue
        if response_kind(response) == PACKET_ACK_START and len(response) == 6:
            return attempt
        if response_kind(response) == PACKET_NACK:
            reason, _ = parse_nack(response)
            raise ProtocolError(f"START rejected: reason={reason}")
    raise TimeoutError("START ACK was not received")


def transfer_image(ser, payload: bytes, packet_retries: int, frame_retries: int) -> tuple[str, float, float, float, int, int]:
    image_crc = zlib.crc32(payload) & 0xFFFFFFFF
    total_packet_retries = 0
    started = time.monotonic()

    for frame_attempt in range(frame_retries + 1):
        session_id = secrets.randbits(32) or 1
        total_packet_retries += wait_for_start_ack(ser, session_id, image_crc, packet_retries)
        restart_frame = False

        for sequence in range(CHUNK_COUNT):
            chunk = payload[sequence * CHUNK_BYTES:(sequence + 1) * CHUNK_BYTES]
            for packet_attempt in range(packet_retries + 1):
                send_packet(ser, packet_data(session_id, sequence, chunk))
                try:
                    response = read_response(ser, session_id, 1.5)
                except TimeoutError:
                    total_packet_retries += 1
                    continue

                kind = response_kind(response)
                if sequence == CHUNK_COUNT - 1 and kind == PACKET_RESULT:
                    predicted, inference_ms, preprocess_ms, _ = parse_result(response)
                    return (
                        predicted,
                        inference_ms,
                        preprocess_ms,
                        (time.monotonic() - started) * 1000.0,
                        frame_attempt,
                        total_packet_retries,
                    )
                if kind == PACKET_ACK_DATA and len(response) == 8:
                    _, _, _, acknowledged = struct.unpack("<BBIH", response)
                    if acknowledged == sequence and sequence != CHUNK_COUNT - 1:
                        total_packet_retries += packet_attempt
                        break
                elif kind == PACKET_NACK:
                    reason, expected = parse_nack(response)
                    if expected == sequence and packet_attempt < packet_retries:
                        total_packet_retries += 1
                        continue
                    if reason in (6, 7):
                        restart_frame = True
                        break
                else:
                    raise ProtocolError(f"unexpected response type={kind} for chunk={sequence}")
            else:
                restart_frame = True

            if restart_frame:
                send_packet(ser, packet_abort(session_id))
                break
        if restart_frame:
            continue
    raise TimeoutError("image transfer failed after frame retries")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("input_path", type=Path)
    parser.add_argument("--baud", type=int, default=1000000)
    parser.add_argument("--packet-retries", type=int, default=3)
    parser.add_argument("--frame-retries", type=int, default=2)
    parser.add_argument("--limit", type=int, help="Maximum number of input images to send")
    parser.add_argument("--quiet", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.packet_retries < 0 or args.frame_retries < 0:
        raise SystemExit("retry counts must be non-negative")
    if args.limit is not None and args.limit <= 0:
        raise SystemExit("--limit must be positive")
    try:
        import serial
    except ImportError as exc:
        raise SystemExit("pyserial is required: python -m pip install pyserial") from exc

    records: list[Record] = []
    with serial.Serial(args.port, args.baud, timeout=0.1) as ser:
        time.sleep(0.2)
        ser.reset_input_buffer()
        image_paths = collect_images(args.input_path.resolve())
        if args.limit is not None:
            image_paths = image_paths[:args.limit]
        for path in image_paths:
            payload = load_image(path)
            predicted, inference_ms, preprocess_ms, round_trip_ms, frame_retries, packet_retries = transfer_image(
                ser, payload, args.packet_retries, args.frame_retries
            )
            record = Record(
                path, path.parent.name, predicted, inference_ms, preprocess_ms,
                round_trip_ms, frame_retries, packet_retries
            )
            records.append(record)
            if not args.quiet:
                verdict = "OK" if record.correct else "MISS"
                print(
                    f"{path}: expected={record.expected} predicted={record.predicted} {verdict} "
                    f"npu_ms={record.inference_ms:.3f} prep_ms={record.preprocess_ms:.3f} "
                    f"round_trip_ms={record.round_trip_ms:.3f} frame_retries={record.frame_retries} "
                    f"packet_retries={record.packet_retries}",
                    flush=True,
                )

    correct = sum(record.correct for record in records)
    times = [record.round_trip_ms for record in records]
    print("\nSummary")
    print(
        f"images={len(records)} correct={correct} accuracy={100.0 * correct / len(records):.2f}% "
        f"round_trip_avg={statistics.fmean(times):.3f} ms "
        f"round_trip_min={min(times):.3f} ms round_trip_max={max(times):.3f} ms"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
