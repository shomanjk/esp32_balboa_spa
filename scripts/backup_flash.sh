#!/usr/bin/env bash
# Full flash dump (ESP32 4 MB) for M5 Atom / spa_module partition layout.
# Usage: ./scripts/backup_flash.sh /dev/cu.usbserial-XXXX
#    or: UPLOAD_PORT=/dev/cu.usbserial-XXXX ./scripts/backup_flash.sh

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ESPTOOL="${HOME}/.platformio/packages/tool-esptoolpy/esptool.py"
PORT="${1:-${UPLOAD_PORT:-}}"

if [[ ! -f "$ESPTOOL" ]]; then
  echo "esptool not found at $ESPTOOL — run a PlatformIO build once so the toolchain is installed."
  exit 1
fi

if [[ -z "$PORT" ]]; then
  echo "Usage: $0 <serial-port>"
  echo "Example: $0 /dev/cu.usbserial-110"
  echo "List ports: pio device list"
  exit 1
fi

if [[ "$PORT" == *YOUR_PORT* ]]; then
  echo "You used the docs placeholder — substitute your real device."
  echo "Plug in the M5 Atom, then run:  pio device list"
  echo "Pick the /dev/cu.… line for the USB serial adapter (not Bluetooth)."
  exit 1
fi

if [[ ! -e "$PORT" ]]; then
  echo "Port not found: $PORT"
  echo "Plug in the Atom USB, then:  pio device list"
  exit 1
fi

mkdir -p "${ROOT}/backups"
OUT="${ROOT}/backups/flash-backup-$(date +%Y%m%d-%H%M%S).bin"

# 921600 often fails on USB–serial bridges; override with e.g. ESPTOOL_BAUD=115200 for reliability
ESPTOOL_BAUD="${ESPTOOL_BAUD:-460800}"

echo "Reading 4 MiB from $PORT into $OUT (baud=$ESPTOOL_BAUD) ..."
python3 "$ESPTOOL" --chip esp32 --port "$PORT" --baud "$ESPTOOL_BAUD" read_flash 0 0x400000 "$OUT"
echo "Done. Backup: $OUT"
ls -la "$OUT"
