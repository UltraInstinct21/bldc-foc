"""Runs one test at a time and streams its progress to the UI."""
from __future__ import annotations

import asyncio
import re
import sys
import time
from pathlib import Path
from typing import Any, Optional

from . import catalog
from .hub import DRIVE_LAB, Hub
from .protocol import Msg
from .sim import SimLink

FIRMWARE = Path(__file__).resolve().parents[2] / "firmware"
LOG_MAX = 500


class JobError(Exception):
    pass


class Jobs:
    def __init__(self, hub: Hub) -> None:
        self.hub = hub
        self.task: Optional[asyncio.Task] = None
        self.job: Optional[dict] = None
        self.history: list[dict] = []

    # ---- public ----------------------------------------------------------------
    def start(self, test_id: str, values: dict[str, Any]) -> dict:
        if self.task and not self.task.done():
            raise JobError("a test is already running")
        t = catalog.get(test_id)
        if t["kind"] == "lab":
            vals = {i["key"]: values.get(i["key"], i["default"]) for i in t.get("inputs", [])}
        else:
            vals = {"secs": values.get("secs", t["secs"])}
        self.job = {"id": f"{test_id}-{int(time.time())}", "test": test_id, "title": t["title"],
                    "status": "running", "step": 0, "steps": 0, "phase": "starting",
                    "log": [], "results": [], "metrics": [], "values": vals,
                    "started": time.time(), "ended": None, "verdict": None}
        self.hub.job = self._brief()
        self.task = asyncio.create_task(self._run(t, vals))
        self._push()
        return self.job

    def flash(self, app: str) -> dict:
        """Not a test: flash an app (e.g. back to the drive lab) and keep it."""
        if self.task and not self.task.done():
            raise JobError("a test is already running")
        self.job = {"id": f"flash-{app}-{int(time.time())}", "test": f"flash:{app}", "title": f"Flash {app}",
                    "status": "running", "step": 0, "steps": 1, "phase": "flashing", "log": [],
                    "results": [], "metrics": [], "values": {}, "started": time.time(),
                    "ended": None, "verdict": None}
        self.task = asyncio.create_task(self._flash_only(app))
        self._push()
        return self.job

    def cancel(self) -> None:
        if self.task and not self.task.done():
            self.task.cancel()

    # ---- plumbing ----------------------------------------------------------------
    def _brief(self) -> Optional[dict]:
        j = self.job
        return None if j is None else {k: j[k] for k in ("id", "test", "title", "status", "phase", "step", "steps")}

    def _push(self) -> None:
        self.hub.job = self._brief() if self.job and self.job["status"] == "running" else None
        self.hub.publish({"type": "job", "data": self.job})

    def _log(self, line: str) -> None:
        self.job["log"].append(line)
        if len(self.job["log"]) > LOG_MAX:
            del self.job["log"][: len(self.job["log"]) - LOG_MAX]
        self.hub.publish({"type": "job_log", "id": self.job["id"], "line": line})

    def _phase(self, phase: str, step: Optional[int] = None) -> None:
        self.job["phase"] = phase
        if step is not None:
            self.job["step"] = step
        self._push()

    async def _finish(self, status: str, verdict: Optional[str]) -> None:
        self.job["status"] = status
        self.job["verdict"] = verdict
        self.job["ended"] = time.time()
        self.job["phase"] = status
        self._push()
        self.history.insert(0, {k: v for k, v in self.job.items() if k != "log"})
        del self.history[30:]

    # ---- waiting on the board ------------------------------------------------------
    async def _until(self, q: asyncio.Queue, pred, timeout: float) -> Optional[Msg]:
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                return None
            try:
                m = await asyncio.wait_for(q.get(), left)
            except asyncio.TimeoutError:
                return None
            if m.kind == "console":
                self._log(m.data)
            if pred(m):
                return m

    # ---- lab procedures ------------------------------------------------------------
    async def _run(self, t: dict, vals: dict) -> None:
        q: asyncio.Queue = asyncio.Queue()
        self.hub.taps.add(q)
        try:
            if t["kind"] == "lab":
                await self._run_lab(t, vals, q)
            else:
                await self._run_app(t, vals, q)
        except asyncio.CancelledError:
            self.hub.esc("test cancelled")
            await self._finish("cancelled", None)
        except Exception as e:
            self.hub.esc("test error")
            self._log(f"!! {e}")
            await self._finish("error", str(e))
        finally:
            self.hub.taps.discard(q)
            self.hub.paused = False

    async def _run_lab(self, t: dict, vals: dict, q: asyncio.Queue) -> None:
        if self.hub.app != DRIVE_LAB:
            raise JobError("the drive lab (test_08_drive_lab) is not running: flash it first")
        steps = t["build"](vals)
        self.job["steps"] = len(steps)
        h = self.hub
        h.send("stop")
        await asyncio.sleep(0.1)
        failed = False
        for n, st in enumerate(steps, 1):
            kind = st[0]
            if kind == "cmd":
                self._phase(st[1], n)
                h.send(st[1])
                if await self._until(q, lambda m: m.kind == "params", 2.5) is None:
                    raise JobError(f"no acknowledgement for '{st[1]}'")
            elif kind == "csa":
                self._phase("CSA offsets", n)
                h.send("csa")
                m = await self._until(q, lambda m: m.kind == "csa", 3)
                if m is None:
                    raise JobError("no CSA reply")
                self.job["results"].append({"kind": "csa", **m.data})
                failed |= not m.data.get("valid", 1)
            elif kind == "raw":
                self._phase(f"raw {st[1]}", n)
                h.send(f"raw {st[1]}")
                m = await self._until(q, lambda m: m.kind == "raw" or (m.kind == "console" and "limited" in m.data), 5)
                if m is None or m.kind != "raw":
                    raise JobError(f"raw {st[1]}: no result")
                self.job["results"].append({"kind": "raw", **m.data})
                await asyncio.sleep(0.4)
            elif kind == "rl":
                self._phase("measuring R and L", n)
                h.send("rl")
                m = await self._until(q, lambda m: m.kind == "rl", 12)
                if m is None:
                    raise JobError("rl: no result")
                self.job["results"].append({"kind": "rl", **m.data})
                failed |= not m.data.get("ok")
            elif kind == "run":
                _, start, secs = st
                self._phase(f"running '{start}' for up to {secs:g} s", n)
                h.send(start)
                m = await self._until(q, lambda m: m.kind == "result" or (
                    m.kind == "console" and any(s in m.data for s in (
                        "run 'rl' first", "would not clear", "out of range"))), float(secs))
                if m is None:
                    self._log(f"-- {secs:g} s elapsed: stopping")
                    h.esc("test time elapsed")
                    m = await self._until(q, lambda m: m.kind == "result", 3)
                if m is None or m.kind != "result":
                    raise JobError(m.data if m else "no result after stop")
                self.job["results"].append({"kind": "result", "start": start, **m.data})
                why = m.data.get("why", "")
                failed |= not any(w in why for w in ("ESC", "stop", "elapsed"))
                self._phase("coasting down", n)
                await asyncio.sleep(2.0)
            elif kind == "sleep":
                await asyncio.sleep(st[1])
            self._push()
        await self._finish("failed" if failed else "passed",
                           "a step stopped on a fault" if failed else "all steps completed")

    # ---- board applications --------------------------------------------------------
    async def _flash(self, app: str) -> None:
        h = self.hub
        self._phase(f"building {app}")
        if isinstance(h.link, SimLink):
            self._log(f"$ make APP={app}   (simulator: skipped)")
            await asyncio.sleep(0.6)
            self._phase(f"flashing {app}")
            self._log("Flashing over USB DFU... (simulated)")
            await asyncio.sleep(0.8)
            h.link.flash(app)
            h.publish({"type": "status", **h.status()})
            self._log("Flash OK")
            await asyncio.sleep(0.2)
            await h.init_lab()
            return
        h.paused = True
        h.mode = "auto"
        h.close_link()
        h.publish({"type": "status", **h.status()})
        await self._sh(["make", f"APP={app}"], f"make APP={app}")
        self._phase(f"flashing {app}")
        await self._sh([sys.executable, "tools/flash.py", "--mode", "auto",
                        "--bin", f"build/{app}/{app}.bin", "--elf", f"build/{app}/{app}.elf"], "flash")
        h.paused = False                     # the supervisor connects the new port in <0.2 s

    async def _sh(self, argv: list[str], what: str) -> None:
        self._log("$ " + " ".join(argv))
        p = await asyncio.create_subprocess_exec(*argv, cwd=FIRMWARE, stdout=asyncio.subprocess.PIPE,
                                                 stderr=asyncio.subprocess.STDOUT)
        assert p.stdout
        async for raw in p.stdout:
            line = raw.decode(errors="replace").rstrip()
            if line and not line.startswith("arm-none-eabi-gcc"):
                self._log(line)
        rc = await p.wait()
        if rc != 0:
            raise JobError(f"{what} failed (exit {rc})")

    async def _wait_app(self, app: str, timeout: float = 12) -> None:
        end = time.time() + timeout
        while time.time() < end:
            if self.hub.app == app and self.hub.link and self.hub.link.alive:
                return
            await asyncio.sleep(0.05)
        raise JobError(f"{app} did not appear on USB after flashing")

    async def _run_app(self, t: dict, vals: dict, q: asyncio.Queue) -> None:
        app = t["app"]
        self.job["steps"] = 3
        await self._flash(app)
        self._phase("waiting for the console", 1)
        await self._wait_app(app)
        self._phase(f"capturing output for {vals['secs']:g} s", 2)
        lines: list[str] = []
        end = time.time() + float(vals["secs"])
        while time.time() < end:
            try:
                m = await asyncio.wait_for(q.get(), max(0.01, end - time.time()))
            except asyncio.TimeoutError:
                break
            if m.kind == "console":
                lines.append(m.data)
                self._log(m.data)
                self._extract(t, lines)
        if t["safety"] != "safe":
            self.hub.esc("test finished")
        self._phase("evaluating", 3)
        text = "\n".join(lines)
        verdict = "passed" if t["pass"] and re.search(t["pass"], text) else None
        if t["fail"] and re.search(t["fail"], text):
            verdict = "failed"
        if verdict is None:
            verdict = "done" if lines else "failed"
        await self._finish(verdict, None if lines else "no console output")

    def _extract(self, t: dict, lines: list[str]) -> None:
        text = "\n".join(lines)
        out = []
        for label, rx, unit in t["metrics"]:
            ms = re.findall(rx, text)
            if ms:
                out.append({"label": label, "value": ms[-1], "unit": unit})
        if out != self.job["metrics"]:
            self.job["metrics"] = out
            self._push()

    async def _flash_only(self, app: str) -> None:
        try:
            await self._flash(app)
            self._phase("waiting for the console", 1)
            await self._wait_app(app)
            await self._finish("done", f"{app} running")
        except asyncio.CancelledError:
            await self._finish("cancelled", None)
        except Exception as e:
            self._log(f"!! {e}")
            await self._finish("error", str(e))
        finally:
            self.hub.paused = False
