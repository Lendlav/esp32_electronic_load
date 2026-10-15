#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 PORT (for example /dev/ttyACM0)" >&2
  exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
cd "$SCRIPT_DIR"
PORT="$1"
PYTHON=python3
if [[ -x .venv/bin/python ]]; then
  PYTHON=.venv/bin/python
fi

"$PYTHON" -m esptool --chip esp32c3 --port "$PORT" --baud 460800 \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x0 electronic_load_esp32c3_full.bin
