#!/usr/bin/env python3
"""Flash a firmware .bin onto the STM32G431 board over USB DFU.

    python3 flash.py release/six_step_v1.bin

Works when the board is running any of our firmwares (it is asked over its
USB serial port to reboot into ST's DFU bootloader) or is already in DFU
mode (BOOT0 high + reset). Needs `dfu-util` (sudo apt install dfu-util).
Close minicom / phase_test.py first: they hold the serial port.
"""
import glob
import os
import shutil
import subprocess
import sys
import time

DFU_ID = "0483:df11"
APP_GLOB = "/dev/serial/by-id/usb-Custom_STM32G431_*-if00"
FLASH_ADDR = "0x08000000"


def in_dfu():
    r = subprocess.run(["lsusb", "-d", DFU_ID], capture_output=True)
    return r.returncode == 0


def wait_for(cond, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.25)
    return False


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    image = sys.argv[1]
    if not os.path.isfile(image):
        sys.exit(f"no such file: {image}")
    size = os.path.getsize(image)
    if not 1024 <= size <= 128 * 1024:
        sys.exit(f"{image}: {size} bytes does not look like a firmware image (1 KB..128 KB)")
    if shutil.which("dfu-util") is None:
        sys.exit("dfu-util not found: sudo apt install dfu-util")

    if not in_dfu():
        ports = glob.glob(APP_GLOB)
        if not ports:
            sys.exit("Board not found: not in DFU mode and no app serial port.\n"
                     "Check the USB cable, or hold BOOT0 high and reset.")
        print(f"App running on {os.path.basename(ports[0])}: asking it to reboot into DFU")
        try:
            with open(ports[0], "wb", buffering=0) as tty:
                tty.write(b"\r" + b"dfu\r")
        except OSError as e:
            sys.exit(f"cannot open {ports[0]}: {e} (is minicom still open?)")
        if not wait_for(in_dfu, 10):
            sys.exit("Board did not enter DFU mode. Hold BOOT0 high and reset, then retry.")
        time.sleep(0.5)

    print(f"Flashing {image} ({size} bytes)...")
    r = subprocess.run(["dfu-util", "-a", "0", "-s", f"{FLASH_ADDR}:leave", "-D", image],
                       capture_output=True, text=True)
    log = r.stdout + r.stderr
    if "File downloaded successfully" not in log:
        print(log)
        sys.exit("Flash FAILED")
    # dfu-util always complains about get_status after the :leave jump; that is normal.

    print("Flash OK, waiting for the new firmware to start...")
    if wait_for(lambda: glob.glob(APP_GLOB), 10):
        print(f"Running: {os.path.basename(glob.glob(APP_GLOB)[0])}")
    else:
        print("Flashed, but no app serial port appeared (fine if this image has no USB).")


if __name__ == "__main__":
    main()
