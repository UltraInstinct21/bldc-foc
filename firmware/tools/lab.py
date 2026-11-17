#!/usr/bin/env python3
"""Drive test_08_drive_lab over its USB console and log everything.

    python3 tools/lab.py --log ../docs/results/run.log  step [step ...]

Steps (run in the order given):
    raw        static duty table (bring-up reproduction), per-phase current
    6step sine svpwm dpwm   open-loop ramp, ~15 s each, then stop
    rl         measure R and L
    if         I/f (closed-loop current, open-loop angle) to 60 Hz, lambda
    sl         sensorless FOC: I/f start, handover, speed loop
    cmd:<text> send any console line, e.g. cmd:vbus 9  cmd:set focmod svpwm

The board must already run test_08_drive_lab (make APP=test_08_drive_lab flash).
ESC is sent on exit, so the bridge is always left off.
"""
import argparse
import fcntl
import glob
import os
import struct
import sys
import termios
import time

PORT_GLOB = "/dev/serial/by-id/usb-Custom_STM32G431_test_08_drive_lab_*-if00"


class Console:
    def __init__(self, path, log):
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        a = termios.tcgetattr(self.fd)
        a[0] = 0; a[1] = 0; a[3] = 0
        a[2] |= termios.HUPCL | termios.CLOCAL | termios.CREAD
        termios.tcsetattr(self.fd, termios.TCSANOW, a)
        fcntl.ioctl(self.fd, termios.TIOCMBIS, struct.pack("I", 0x002))   # DTR
        self.log = log
        self.buf = ""

    def _pump(self):
        try:
            data = os.read(self.fd, 4096).decode(errors="replace")
        except BlockingIOError:
            return False
        data = data.replace("\r", "")
        sys.stdout.write(data); sys.stdout.flush()
        self.log.write(data); self.log.flush()
        self.buf += data
        return True

    def send(self, line):
        note = f"\n>>> {line}\n"
        sys.stdout.write(note); self.log.write(note)
        os.write(self.fd, (line + "\r").encode())

    def wait(self, needles, timeout):
        """Read until any needle appears in a new line, or timeout. Returns needle or None."""
        self.buf = ""
        end = time.time() + timeout
        while time.time() < end:
            if not self._pump():
                time.sleep(0.02)
            for n in needles:
                if n in self.buf:
                    return n
        return None

    def esc(self):
        os.write(self.fd, b"\x1b")
        time.sleep(0.3)
        self._pump()

    def close(self):
        os.close(self.fd)


RAW_TABLE = ["10 0 0", "12 0 6", "12 2 6", "26 14 20", "56 44 50", "55 45 50"]


def run_open(c, mode, secs=15):
    for cmd in (f"mode {mode}", "amp 40", "f 200"):
        c.send(cmd); c.wait(["ok", "?"], 2)
    c.send("start")
    if c.wait(["RESULT"], secs) is None:
        c.send("stop")
        c.wait(["RESULT"], 3)
    c.wait(["ready"], 2)
    time.sleep(2.0)                  # let the rotor coast down


def run_foc(c, kind, f, secs, extra=()):
    for cmd in (f"f {f}",) + tuple(extra):
        c.send(cmd); c.wait(["ok", "?"], 2)
    c.send(kind)
    if c.wait(["RESULT", "run 'rl' first"], secs) is None:
        c.send("stop")
        c.wait(["RESULT"], 3)
    c.wait(["ready"], 2)
    time.sleep(2.0)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--log", required=True)
    ap.add_argument("steps", nargs="+")
    a = ap.parse_args()

    ports = glob.glob(PORT_GLOB)
    if not ports:
        sys.exit("test_08_drive_lab is not running (make APP=test_08_drive_lab flash)")
    os.makedirs(os.path.dirname(os.path.abspath(a.log)), exist_ok=True)
    with open(a.log, "a") as log:
        log.write(f"\n===== {time.strftime('%Y-%m-%d %H:%M:%S')} steps: {' '.join(a.steps)}\n")
        c = Console(ports[0], log)
        try:
            c.send("stop"); c.wait(["ok"], 2)
            c.send("params"); c.wait(["ok"], 2)
            for step in a.steps:
                if step == "raw":
                    for r in RAW_TABLE:
                        c.send(f"raw {r}"); c.wait(["ready"], 5); time.sleep(0.5)
                elif step in ("6step", "sine", "svpwm", "dpwm"):
                    run_open(c, step)
                elif step == "rl":
                    c.send("rl"); c.wait(["ready"], 10)
                elif step == "if":
                    run_foc(c, "if", 60, 15)
                elif step == "sl":
                    run_foc(c, "sl", 120, 25)
                elif step.startswith("trace"):
                    c.send(step.replace(":", " ")); c.wait(["ok"], 8)
                elif step.startswith("cmd:"):
                    c.send(step[4:]); c.wait(["ok", "ready", "?"], 3)
                else:
                    print(f"unknown step {step}")
        finally:
            c.esc()
            c.close()


if __name__ == "__main__":
    main()
