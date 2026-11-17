#!/usr/bin/env python3
"""Send commands to the board's USB serial port and print the replies.

    python3 ser.py status                 one command
    python3 ser.py "duty 10" start @20    commands in order; "@N" = turn the
                                          1 s log on for N seconds and print
                                          only the summary/fault lines
    python3 ser.py stop

Used for every automated test in FIRMWARE_README.md. Close minicom first:
only one program can hold the port.
"""
import glob
import os
import sys
import termios
import time

ports = glob.glob("/dev/serial/by-id/usb-Custom_STM32G431_*-if00")
if not ports:
    sys.exit("board not found on USB")
fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd)
a[3] &= ~(termios.ECHO | termios.ICANON)
a[0] = 0
a[1] = 0
termios.tcsetattr(fd, termios.TCSANOW, a)


def rd(t):
    out = b""
    end = time.time() + t
    while time.time() < end:
        try:
            out += os.read(fd, 4096)
        except BlockingIOError:
            time.sleep(0.02)
    return out.decode(errors="replace")


rd(0.6)                                   # drop the banner / pending output
for c in sys.argv[1:]:
    if c.startswith("@"):
        os.write(fd, b"log\r")
        s = rd(float(c[1:]))
        os.write(fd, b"log\r")
        rd(0.3)
        print("\n".join(l for l in s.splitlines()
                        if l.startswith("[") or "!!" in l or "auto-" in l or "check: MIS" in l))
    else:
        os.write(fd, (c + "\r").encode())
        print(rd(0.8), end="")
os.close(fd)
