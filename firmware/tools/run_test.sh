#!/bin/bash
# Flash an app over USB DFU and capture its console from the very first line.
#   tools/run_test.sh <app> [seconds]          e.g. tools/run_test.sh test_02_drv8316 4
#   STOP_AT_END=1 tools/run_test.sh test_05_open_loop 20   (ESC = bridge off at the end)
# Run from firmware/ after `make APP=<app>`. The board must already be running
# one of this repo's apps (or be in DFU mode).
cd "$(dirname "$0")/.." || exit 1
APP=$1; SECS=${2:-6}
LOG=$(mktemp)
python3 tools/flash.py --mode dfu --bin build/$APP/$APP.bin --elf build/$APP/$APP.elf > "$LOG" 2>&1 &
FL=$!
# 1) wait until the board is in the ROM bootloader (the old app port is gone)
for i in $(seq 1 150); do lsusb -d 0483:df11 >/dev/null 2>&1 && break; sleep 0.1; done
# 2) wait for the new app's port and open it at once (firmware waits up to 3 s for us)
P=""
for i in $(seq 1 200); do
  P=$(ls /dev/serial/by-id/usb-Custom_STM32G431_${APP}_*-if00 2>/dev/null) && [ -n "$P" ] && ! lsusb -d 0483:df11 >/dev/null 2>&1 && break
  P=""; sleep 0.05
done
[ -z "$P" ] && { wait $FL; cat "$LOG"; echo "port for $APP not found"; exit 1; }
python3 -I tools/read_port.py "$P" "$SECS"
wait $FL; echo; echo "[flash: $(grep -E 'Flash OK|FAILED' "$LOG" | head -1)]"
rm -f "$LOG"
