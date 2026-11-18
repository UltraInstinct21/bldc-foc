"""Transport to the board: a USB CDC serial port, or the simulator.

Both expose the same small interface. Each runs its own reader thread and hands
every complete line to `on_line` (called from that thread).
"""
from __future__ import annotations

import glob
import os
import re
import threading
import time
from typing import Callable, Optional

import serial

PORT_GLOB = "/dev/serial/by-id/usb-Custom_STM32G431_*-if00"
_APP_RE = re.compile(r"usb-Custom_STM32G431_(.+?)_[^_]+-if00$")


def list_ports() -> list[dict]:
    out = []
    for p in sorted(glob.glob(PORT_GLOB)):
        m = _APP_RE.search(os.path.basename(p))
        out.append({"path": p, "app": m.group(1) if m else None,
                    "dev": os.path.realpath(p)})
    return out


def app_of(port: str) -> Optional[str]:
    m = _APP_RE.search(os.path.basename(port))
    return m.group(1) if m else None


class Link:
    """Interface shared by SerialLink and SimLink."""
    kind = "none"
    port = ""
    app: Optional[str] = None

    def __init__(self, on_line: Callable[[str], None], on_close: Callable[[str], None]):
        self.on_line = on_line
        self.on_close = on_close

    def start(self) -> None: ...
    def send(self, line: str) -> None: ...
    def esc(self) -> None: ...
    def close(self) -> None: ...
    @property
    def alive(self) -> bool: return False


class SerialLink(Link):
    kind = "serial"

    def __init__(self, port: str, on_line, on_close):
        super().__init__(on_line, on_close)
        self.port = port
        self.app = app_of(port)
        self._ser: Optional[serial.Serial] = None
        self._stop = threading.Event()
        self._wlock = threading.Lock()
        self._thread: Optional[threading.Thread] = None

    def start(self) -> None:
        # The firmware prints only while DTR is set (docs: HANDOFF.md §6.8).
        s = serial.Serial()
        s.port = self.port
        s.baudrate = 115200          # ignored by USB CDC
        s.timeout = 0.05
        s.dtr = True
        s.rts = False
        s.open()
        self._ser = s
        self._thread = threading.Thread(target=self._run, name="serial-reader", daemon=True)
        self._thread.start()

    def _run(self) -> None:
        buf = b""
        why = "closed"
        try:
            while not self._stop.is_set():
                chunk = self._ser.read(4096)
                if not chunk:
                    continue
                buf += chunk
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    line = raw.decode(errors="replace").rstrip("\r")
                    if line:
                        self.on_line(line)
        except (serial.SerialException, OSError) as e:
            why = f"port lost: {e}"
        finally:
            try:
                self._ser.close()
            except Exception:
                pass
            if not self._stop.is_set():
                self.on_close(why)

    def send(self, line: str) -> None:
        if not self.alive:
            raise ConnectionError("not connected")
        with self._wlock:
            self._ser.write((line + "\r").encode())
            self._ser.flush()

    def esc(self) -> None:
        if self.alive:
            with self._wlock:
                self._ser.write(b"\x1b")
                self._ser.flush()

    def close(self) -> None:
        self._stop.set()
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=1.0)

    @property
    def alive(self) -> bool:
        return self._ser is not None and self._ser.is_open and not self._stop.is_set()


def wait_port(timeout: float, app: Optional[str] = None) -> Optional[str]:
    end = time.time() + timeout
    while time.time() < end:
        for p in list_ports():
            if app is None or p["app"] == app:
                return p["path"]
        time.sleep(0.1)
    return None
