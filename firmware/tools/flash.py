#!/usr/bin/env python3
"""Flash a firmware image onto the board.

    python3 tools/flash.py --bin build/foc/foc.bin --elf build/foc/foc.elf [--mode auto|dfu|swd]

auto (default): USB-C first. If the board shows up on USB, either running one
of our apps (USB serial "STM32G431 ...") or already in the ST ROM bootloader
(0483:df11), it is flashed with dfu-util. Otherwise ST-Link/SWD via OpenOCD.

USB DFU flow (verified on the board): the running app is sent "dfu" on its
USB serial port, reboots into the ROM bootloader, dfu-util writes the image
with :leave, and the new app starts. No BOOT0 jumper is needed once any of
these apps is running. For a blank chip or a crashed app: BOOT0 (pin 45, PB8)
to 3V3, replug USB, flash, remove the jumper.

Close minicom/picocom first: they hold the serial port.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
import time

DFU_ID = "0483:df11"
APP_GLOB = "/dev/serial/by-id/usb-Custom_STM32G431_*-if00"
FLASH_ADDR = "0x08000000"


def lsusb_has(vid_pid):
    if shutil.which("lsusb") is None:
        return False
    return subprocess.run(["lsusb", "-d", vid_pid], capture_output=True).returncode == 0


def in_dfu():
    return lsusb_has(DFU_ID)


def app_ports():
    return glob.glob(APP_GLOB)


def wait_for(cond, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.25)
    return False


def flash_dfu(image):
    size = os.path.getsize(image)
    if not 1024 <= size <= 128 * 1024:
        sys.exit(f"{image}: {size} bytes does not look like a firmware image (1 KB..128 KB)")
    if shutil.which("dfu-util") is None:
        sys.exit("dfu-util not found: sudo apt install dfu-util")

    if not in_dfu():
        ports = app_ports()
        if not ports:
            sys.exit("Board not on USB: not in DFU mode and no app serial port.\n"
                     "Check the cable (C-to-A works best, see docs/hardware.md), or\n"
                     "tie BOOT0 (pin 45) to 3V3 and replug, or use --mode swd.")
        print(f"App running on {os.path.basename(ports[0])}: asking it to reboot into DFU")
        try:
            with open(ports[0], "wb", buffering=0) as tty:
                tty.write(b"\rdfu\r")
        except OSError as e:
            sys.exit(f"cannot open {ports[0]}: {e} (is minicom still open?)")
        if not wait_for(in_dfu, 10):
            sys.exit("Board did not enter DFU mode. Tie BOOT0 high and replug, then retry.")
        time.sleep(0.5)

    print(f"Flashing {image} ({size} bytes) over USB DFU...")
    r = subprocess.run(["dfu-util", "-a", "0", "-s", f"{FLASH_ADDR}:leave", "-D", image],
                       capture_output=True, text=True)
    log = r.stdout + r.stderr
    if "File downloaded successfully" not in log:
        print(log)
        sys.exit("Flash FAILED")
    # dfu-util reports "Error during download get_status" after the :leave
    # jump, and "Invalid DFU suffix signature"; both are normal.

    print("Flash OK, waiting for the new firmware to start...")
    if wait_for(app_ports, 10):
        print(f"Running: {os.path.basename(app_ports()[0])}")
    else:
        print("Flashed, but no app serial port appeared yet.")


def flash_swd(elf, openocd, interface, target):
    if shutil.which(openocd.split()[0]) is None:
        sys.exit(f"{openocd} not found: sudo apt install openocd")
    print(f"Flashing {elf} over SWD (OpenOCD, {interface})...")
    cmd = openocd.split() + ["-f", interface, "-f", target,
                             "-c", f"program {elf} verify reset exit"]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit("SWD flash FAILED. J5 has no reset wire: if the probe cannot connect,\n"
                 "short NRST to GND while it connects (the SW4 button is wired to 3V3\n"
                 "and does nothing).")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--elf", required=True)
    ap.add_argument("--mode", choices=["auto", "dfu", "swd"], default="auto")
    ap.add_argument("--openocd", default="openocd")
    ap.add_argument("--interface", default="interface/stlink.cfg")
    ap.add_argument("--target", default="target/stm32g4x.cfg")
    a = ap.parse_args()

    for f in (a.bin, a.elf):
        if not os.path.isfile(f):
            sys.exit(f"no such file: {f}")

    mode = a.mode
    if mode == "auto":
        if in_dfu() or app_ports():
            print("Board found on USB-C: using DFU.")
            mode = "dfu"
        else:
            print("Board not found on USB-C: falling back to ST-Link/SWD.")
            mode = "swd"

    if mode == "dfu":
        flash_dfu(a.bin)
    else:
        flash_swd(a.elf, a.openocd, a.interface, a.target)


if __name__ == "__main__":
    main()
