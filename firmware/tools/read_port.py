#!/usr/bin/env python3
"""Read a board's USB console for N seconds, keeping DTR asserted on one handle.

    python3 tools/read_port.py /dev/serial/by-id/usb-Custom_STM32G431_<app>_*-if00 5
    STOP_AT_END=1 python3 tools/read_port.py <port> 20      # send ESC (bridge off) at the end

Why not `cat`/`stty`: opening and closing the port toggles DTR; the firmware
treats DTR as "terminal open" and output written while it is low is lost.
"""
import fcntl
import os
import struct
import sys
import termios
import time

p, secs = sys.argv[1], float(sys.argv[2])
fd = os.open(p, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd); a[0] = 0; a[1] = 0; a[3] = 0
a[2] |= termios.HUPCL | termios.CLOCAL | termios.CREAD
termios.tcsetattr(fd, termios.TCSANOW, a)
fcntl.ioctl(fd, termios.TIOCMBIS, struct.pack('I', 0x002))   # DTR on
t = time.time()
while time.time() - t < secs:
    try:
        sys.stdout.write(os.read(fd, 4096).decode(errors='replace')); sys.stdout.flush()
    except BlockingIOError:
        time.sleep(0.02)
if os.environ.get('STOP_AT_END'):                            # ESC: bridge off
    os.write(fd, b'\x1b'); time.sleep(0.3)
    try:
        sys.stdout.write(os.read(fd, 4096).decode(errors='replace'))
    except BlockingIOError:
        pass
os.close(fd)                                                 # HUPCL drops DTR
