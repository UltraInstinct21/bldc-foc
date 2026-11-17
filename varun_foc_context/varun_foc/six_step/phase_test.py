#!/usr/bin/env python3
"""Interactive phase / phase-pair test for the 6-step board.

Keys (no Enter needed):
  a b      pair A-B : A switching (PWM), B held low, C off   -> measure across A-B
  b c      pair B-C        a c   pair A-C
  b a      reversed polarity (B switching, A low), same for c b / c a
  a SPACE  phase A alone (B, C off)                          -> measure A vs PGND
  z        all phases off
  + / -    duty up / down by 5 %
  s        board status
  q        quit (board goes back to normal 6-step commutation)

Only one program can hold the serial port: close minicom first.
"""
import glob
import os
import select
import sys
import termios
import time
import tty

PORT_GLOB = "/dev/serial/by-id/usb-Custom_STM32G431_*-if00"


def open_port():
    ports = glob.glob(PORT_GLOB)
    if not ports:
        sys.exit("Board not found on USB (no " + PORT_GLOB + ")")
    fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = 0
    a[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG)
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def drain(fd, seconds, show=True):
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.05)
        if r:
            try:
                data = os.read(fd, 4096)
            except BlockingIOError:
                continue
            if show:
                sys.stdout.write(data.decode(errors="replace").replace("\r\n", "\n"))
                sys.stdout.flush()


def send(fd, cmd, wait=2.0):
    os.write(fd, (cmd + "\r").encode())
    drain(fd, wait)


def main():
    fd = open_port()
    drain(fd, 0.5, show=False)            # discard banner / pending output
    print(__doc__)
    send(fd, "ph off")

    stdin = sys.stdin.fileno()
    old = termios.tcgetattr(stdin)
    tty.setcbreak(stdin)
    first = None
    try:
        while True:
            r, _, _ = select.select([stdin, fd], [], [], 0.1)
            if fd in r:
                drain(fd, 0.05)
            if stdin not in r:
                continue
            k = os.read(stdin, 1).decode(errors="ignore").lower()
            if k == "q" or k == "\x03":
                break
            if first:                        # second key of a pair
                if k in "abc" and k != first:
                    send(fd, "ph " + first + k)
                elif k in " \n\r":
                    send(fd, "ph " + first)
                else:
                    print(f"\n  ({first.upper()} cancelled)")
                first = None
            elif k in "abc":
                first = k
                print(f"\n  {k.upper()} + ?  (second phase letter for a pair, SPACE for {k.upper()} alone)")
            elif k == "z":
                send(fd, "ph off")
            elif k in "+-":
                send(fd, "ph " + k)
            elif k == "s":
                send(fd, "status", 1.0)
    finally:
        termios.tcsetattr(stdin, termios.TCSADRAIN, old)
        print("\nleaving phase test...")
        send(fd, "ph exit", 1.5)
        os.close(fd)


if __name__ == "__main__":
    main()
