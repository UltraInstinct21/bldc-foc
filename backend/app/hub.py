"""The hub: owns the link to the board, the latest state, and the fan-out to
WebSocket clients and jobs.

Threading: links call `_on_line` from their reader thread; everything else
runs on the asyncio loop. `_on_line` only hops onto the loop.
"""
from __future__ import annotations

import asyncio
import csv
import time
from collections import deque
from pathlib import Path
from typing import Any, Optional

from .link import Link, SerialLink, app_of, list_ports
from .protocol import Msg, parse_line, wave_from_rows
from .sim import SimLink

DRIVE_LAB = "test_08_drive_lab"
REC_DIR = Path(__file__).resolve().parent.parent / "recordings"
DEADMAN_S = 2.0


class Hub:
    def __init__(self) -> None:
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self.link: Optional[Link] = None
        self.mode = "auto"              # auto | serial | sim | off
        self.want_port: Optional[str] = None
        self.paused = False             # true while a flash owns the port
        self.clients: set[asyncio.Queue] = set()
        self.taps: set[asyncio.Queue] = set()   # jobs listening to every Msg
        self.console: deque[str] = deque(maxlen=600)
        self.tel: Optional[dict] = None
        self.params: Optional[dict] = None
        self.hello: Optional[dict] = None
        self.csa: Optional[dict] = None
        self.wave: Optional[dict] = None         # last scope snapshot, for late joiners
        self.last_error = ""
        self._wave_hdr: Optional[dict] = None
        self._wave_rows: list[list[int]] = []
        self._rec_file = None
        self._rec_writer = None
        self.rec_name: Optional[str] = None
        self.rec_rows = 0
        self._no_client_since: Optional[float] = time.time()
        self.job: Optional[dict] = None     # set by jobs.Jobs

    # ---- lifecycle -------------------------------------------------------------
    async def run(self) -> None:
        self.loop = asyncio.get_running_loop()
        while True:
            try:
                await self._tick()
            except Exception as e:      # never let the supervisor die
                self.last_error = f"supervisor: {e}"
            await asyncio.sleep(0.2)

    async def _tick(self) -> None:
        # connect / reconnect
        if not self.paused and (self.link is None or not self.link.alive):
            if self.mode == "sim":
                await self._open(SimLink(self._on_line, self._on_close))
            elif self.mode in ("auto", "serial"):
                ports = list_ports()
                path = None
                if self.mode == "serial" and self.want_port:
                    path = self.want_port if any(p["path"] == self.want_port for p in ports) else None
                elif ports:
                    lab = [p for p in ports if p["app"] == DRIVE_LAB]
                    path = (lab or ports)[0]["path"]
                if path:
                    try:
                        await self._open(SerialLink(path, self._on_line, self._on_close))
                    except Exception as e:
                        self.last_error = f"open {path}: {e}"
        # deadman: the bridge must not run with nobody watching
        if self.clients:
            self._no_client_since = None
        elif self._no_client_since is None:
            self._no_client_since = time.time()
        elif (time.time() - self._no_client_since > DEADMAN_S and self.running
              and not (self.job and self.job.get("headless"))):
            self.esc("deadman: no UI connected")

    async def _open(self, link: Link) -> None:
        self.link = link
        link.start()
        self.tel = self.hello = None
        self.last_error = ""
        self.publish({"type": "status", **self.status()})
        await asyncio.sleep(0.3)
        await self.init_lab()

    async def init_lab(self) -> None:
        """Put a freshly started drive lab into machine mode (RAM settings are
        lost on every reset/flash)."""
        if self.link and self.link.alive and self.link.app == DRIVE_LAB:
            for cmd in ("echo off", "stop", "hello", "tel 50", "log 1000", "params", "csa"):
                self.send(cmd)
                await asyncio.sleep(0.03)

    def _on_close(self, why: str) -> None:
        if self.loop:
            self.loop.call_soon_threadsafe(self._closed, why)

    def _closed(self, why: str) -> None:
        self.last_error = why
        self.link = None
        self.tel = None
        self.publish({"type": "status", **self.status()})

    async def set_mode(self, mode: str, port: Optional[str] = None) -> None:
        self.mode = mode
        self.want_port = port
        self.close_link()
        self.publish({"type": "status", **self.status()})

    def close_link(self) -> None:
        if self.link:
            try:
                if self.running:
                    self.link.esc()
            finally:
                self.link.close()
                self.link = None
                self.tel = None

    # ---- incoming lines ----------------------------------------------------------
    def _on_line(self, line: str) -> None:
        if self.loop:
            self.loop.call_soon_threadsafe(self._handle, line)

    def _handle(self, line: str) -> None:
        msg = parse_line(line)
        k, d = msg.kind, msg.data
        if k == "tel":
            self.tel = d
            self._record(d)
            self.publish({"type": "tel", "data": d})
        elif k == "wave_hdr":
            if d.get("end"):
                if self._wave_hdr is not None:
                    self.wave = wave_from_rows(self._wave_hdr, self._wave_rows)
                    self.publish({"type": "wave", "data": self.wave})
                self._wave_hdr, self._wave_rows = None, []
            else:
                self._wave_hdr, self._wave_rows = d, []
        elif k == "wave_row":
            if self._wave_hdr is not None:
                self._wave_rows.append(d)
        else:
            if k == "params":
                self.params = d
            elif k == "hello":
                self.hello = d
            elif k == "csa":
                self.csa = d
            if k == "console":
                if line.strip() in ("ok",):
                    pass                     # acknowledgements: keep the console readable
                else:
                    self.console.append(line)
            self.publish({"type": k, "data": d} if k != "console" else {"type": "console", "line": line})
        for q in list(self.taps):
            q.put_nowait(msg)

    # ---- outgoing ----------------------------------------------------------------
    def send(self, line: str) -> None:
        if not self.link or not self.link.alive:
            raise ConnectionError("board not connected")
        self.link.send(line)
        if line not in ("tel 50", "echo off", "hello"):
            self.publish({"type": "sent", "line": line})

    def esc(self, why: str = "stop") -> None:
        if self.link and self.link.alive:
            self.link.esc()
            try:
                self.link.send("stop")
            except Exception:
                pass
        self.publish({"type": "event", "level": "warn", "text": f"STOP ({why})"})

    @property
    def running(self) -> bool:
        return bool(self.tel and self.tel.get("st") not in (None, "idle"))

    @property
    def app(self) -> Optional[str]:
        return self.link.app if self.link else None

    # ---- fan-out -----------------------------------------------------------------
    def publish(self, ev: dict[str, Any]) -> None:
        for q in list(self.clients):
            if q.qsize() > 500:          # slow client: drop telemetry, keep the rest
                if ev.get("type") == "tel":
                    continue
            q.put_nowait(ev)

    def status(self) -> dict:
        return {
            "connected": bool(self.link and self.link.alive),
            "mode": self.mode,
            "link": self.link.kind if self.link else None,
            "port": self.link.port if self.link else None,
            "app": self.app,
            "paused": self.paused,
            "error": self.last_error,
            "ports": list_ports(),
            "recording": self.rec_name,
            "rec_rows": self.rec_rows,
            "job": self.job,
        }

    def snapshot(self) -> dict:
        return {"type": "snapshot", "status": self.status(), "params": self.params,
                "hello": self.hello, "csa": self.csa, "tel": self.tel, "wave": self.wave,
                "console": list(self.console)[-300:]}

    # ---- recording ---------------------------------------------------------------
    REC_COLS = ["t", "st", "mod", "kind", "obs", "f", "fol", "fpll", "amp", "cap", "ifilt",
                "i0", "i1", "i2", "rms0", "rms1", "rms2", "pk0", "pk1", "pk2", "res", "flow",
                "drop", "id", "iq", "idr", "iqr", "vd", "vq", "lam", "lamu", "d0", "d1", "d2",
                "th", "nf", "fmax", "imax", "stop"]

    def rec_start(self) -> str:
        self.rec_stop()
        REC_DIR.mkdir(exist_ok=True)
        name = time.strftime("tel_%Y%m%d_%H%M%S.csv")
        self._rec_file = open(REC_DIR / name, "w", newline="")
        self._rec_writer = csv.writer(self._rec_file)
        self._rec_writer.writerow(["host_time"] + self.REC_COLS + ["params"])
        self.rec_name, self.rec_rows = name, 0
        self.publish({"type": "status", **self.status()})
        return name

    def rec_stop(self) -> Optional[str]:
        name = self.rec_name
        if self._rec_file:
            self._rec_file.close()
        self._rec_file = self._rec_writer = None
        self.rec_name = None
        if name:
            self.publish({"type": "status", **self.status()})
        return name

    def _record(self, d: dict) -> None:
        if not self._rec_writer:
            return
        row = [f"{time.time():.3f}"]
        for c in self.REC_COLS:
            if c[-1].isdigit() and c[:-1] in ("i", "rms", "pk", "d"):
                arr = d.get(c[:-1]) or []
                n = int(c[-1])
                row.append(arr[n] if n < len(arr) else "")
            else:
                row.append(d.get(c, ""))
        row.append("" if self.rec_rows else str(self.params))
        self._rec_writer.writerow(row)
        self.rec_rows += 1
        if self.rec_rows % 50 == 0:
            self._rec_file.flush()


hub = Hub()

__all__ = ["hub", "Hub", "DRIVE_LAB", "REC_DIR", "Msg", "app_of"]
