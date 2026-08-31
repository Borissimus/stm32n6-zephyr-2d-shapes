# STM32N6 Zephyr Shapes2D

Minimal STM32N6570-DK NPU application for the `circle`, `square`, and
`triangle` Shapes2D classifier. It has no camera or display dependencies.

At boot it starts the LED heartbeat, initializes XSPI2/NPU, runs three
embedded reference images, then accepts `96x96` RGB888 frames over the debug
UART.

## Model Contract

- Input transport: RGB HWC, `96x96x3`, unsigned 8-bit, 27,648 bytes.
- Model input: RGB NCHW int8, normalized from RGB using `(x / 255 - 0.5) / 0.5`.
- Output order: `circle`, `square`, `triangle`.
- Weights: `ai/shapes_mobilenet_v2/shapes_mobilenet_v2_atonbuf.xSPI2.raw` at
  XSPI2 mapped address `0x71000000`.

## Build

Run from the Zephyr workspace root, after its dependencies have been
bootstrapped:

```bash
APP=stm32n6-zephyr-2d-shapes
./stm32n6-zephyr-ai-app/.venv/bin/west build -p always \
  -b stm32n6570_dk//sb -s "$APP" -d "$APP/build-ram"
```

The RAM serial-boot image is written to `build-ram/zephyr/zephyr.signed.bin`.
Before running it, program the model blob through ST-LINK:

```bash
./stm32n6-zephyr-2d-shapes/scripts/program_shapes_weights.sh
```

## UART Test

The UART is configured for `1_000_000` baud. The app accepts the same
verified framed transport as the PSoC Shapes2D application:

- `TST0`: repeat the embedded three-image self-test.
- `IMG0` + raw RGB payload: legacy frame.
- `IMG0` + `VHDR` + `CHNK` packets: verified frame with CRC checks.

Install the sender dependencies on the host and run the complete test set:

```bash
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --upgrade pip pillow pyserial
python3 tools/send_shape_image.py /dev/ttyUSB0 \
  /absolute/path/to/models/shapes2d/datasets/test
```

`tools/generate_embedded_self_test_header.py` regenerates the three embedded
reference inputs from that same dataset. It intentionally embeds one image per
class so that the RAM-load image remains inside the STM32N6 secure-RAM limit.
