#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
APP_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
WEIGHTS_FILE="$APP_DIR/ai/shapes_mobilenet_v2/shapes_mobilenet_v2_atonbuf.xSPI2.raw"
PROGRAMMER_BIN_DIR="${STM32CUBEPROG_DIR:-$HOME/STMicroelectronics/STM32Cube/STM32CubeProgrammer}/bin"

if [[ ! -f "$WEIGHTS_FILE" ]]; then
  echo "Shapes2D weights not found: $WEIGHTS_FILE" >&2
  exit 1
fi
if [[ ! -x "$PROGRAMMER_BIN_DIR/STM32_Programmer_CLI" ]]; then
  echo "STM32_Programmer_CLI not found in $PROGRAMMER_BIN_DIR" >&2
  exit 1
fi

# CubeProgrammer accepts a .bin payload more reliably than a .raw filename.
weights_bin=$(mktemp -t shapes_mobilenet_v2.XXXXXX.bin)
trap 'rm -f "$weights_bin"' EXIT
cp "$WEIGHTS_FILE" "$weights_bin"

env -u LD_LIBRARY_PATH PATH="$PROGRAMMER_BIN_DIR:$PATH" \
  STM32_Programmer_CLI \
  --connect port=swd mode=HOTPLUG ap=1 \
  --extload "$PROGRAMMER_BIN_DIR/ExternalLoader/MX66UW1G45G_STM32N6570-DK.stldr" \
  --download "$weights_bin" 0x71000000 \
  --verify
