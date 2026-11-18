"""HTTP + WebSocket API for the motor-control UI.

    cd backend && uv run uvicorn app.main:app --port 8000
    BLDC_SIM=1 uv run uvicorn app.main:app --port 8000     # start in simulator mode

REST does actions; /ws streams everything (telemetry, params, console, jobs)
and also accepts {"type":"cmd","line":...} and {"type":"stop"} for low latency.
If frontend/dist exists it is served at /, so one process runs the whole UI.
"""
from __future__ import annotations

import asyncio
import os
import re
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, Optional

from fastapi import FastAPI, HTTPException, WebSocket, WebSocketDisconnect
from fastapi.middleware.cors import CORSMiddleware
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

from . import catalog
from .hub import REC_DIR, hub
from .jobs import JobError, Jobs
from .link import list_ports

jobs = Jobs(hub)
DIST = Path(__file__).resolve().parents[2] / "frontend" / "dist"

# Console lines the UI may send. Anything else is refused, so a typo in a
# script cannot reach the bridge as something unexpected.
ALLOWED = re.compile(
    r"^(help|params|stop|start|rl|if|sl|hello|csa|trace( \d+)?|snap( \d+)?|tel \d+|log \d+"
    r"|mode (6step|sine|svpwm|dpwm)"
    r"|raw -?[\d.]+ -?[\d.]+ -?[\d.]+"
    r"|(amp|f|ilim|vbus|fstart|align) -?[\d.]+"
    r"|accel -?[\d.]+ -?[\d.]+"
    r"|set (R|L|lam|bw|ifa|fsw|kps|kis|dtc|pll|ampmax|idsl|vrec|trdec) -?[\d.eE+-]+"
    r"|set focmod (sine|svpwm|dpwm)"
    r"|echo (on|off))$")


@asynccontextmanager
async def lifespan(_: FastAPI):
    if os.environ.get("BLDC_SIM"):
        hub.mode = "sim"
    task = asyncio.create_task(hub.run())
    yield
    task.cancel()
    hub.close_link()
    hub.rec_stop()


app = FastAPI(title="bldc-foc backend", lifespan=lifespan)
app.add_middleware(CORSMiddleware, allow_origins=["http://localhost:5173", "http://127.0.0.1:5173"],
                   allow_methods=["*"], allow_headers=["*"])


class ConnectReq(BaseModel):
    mode: str = "auto"               # auto | serial | sim | off
    port: Optional[str] = None


class CmdReq(BaseModel):
    line: str


class RunReq(BaseModel):
    values: dict[str, Any] = {}


class FlashReq(BaseModel):
    app: str


def _send(line: str) -> None:
    line = line.strip()
    if not ALLOWED.match(line):
        raise HTTPException(400, f"command not allowed: {line!r}")
    try:
        hub.send(line)
    except ConnectionError as e:
        raise HTTPException(409, str(e))


@app.get("/api/status")
def status() -> dict:
    return hub.status()


@app.get("/api/ports")
def ports() -> list[dict]:
    return list_ports()


@app.post("/api/connect")
async def connect(r: ConnectReq) -> dict:
    if r.mode not in ("auto", "serial", "sim", "off"):
        raise HTTPException(400, "mode must be auto, serial, sim or off")
    await hub.set_mode(r.mode, r.port)
    return hub.status()


@app.post("/api/command")
async def command(r: CmdReq) -> dict:
    _send(r.line)
    return {"ok": True}


@app.post("/api/stop")
async def stop() -> dict:
    hub.esc("UI stop")
    jobs.cancel()
    return {"ok": True}


@app.get("/api/tests")
def tests() -> list[dict]:
    return [catalog.public(t) for t in catalog.CATALOG]


@app.post("/api/tests/{test_id}/run")
async def run_test(test_id: str, r: RunReq) -> dict:
    try:
        return jobs.start(test_id, r.values)
    except KeyError:
        raise HTTPException(404, f"no test {test_id}")
    except JobError as e:
        raise HTTPException(409, str(e))


@app.post("/api/flash")
async def flash(r: FlashReq) -> dict:
    if not any(t.get("app") == r.app for t in catalog.CATALOG) and r.app not in ("test_08_drive_lab", "foc_esp"):
        raise HTTPException(400, f"unknown app {r.app}")
    try:
        return jobs.flash(r.app)
    except JobError as e:
        raise HTTPException(409, str(e))


@app.get("/api/jobs")
def job_state() -> dict:
    return {"current": jobs.job, "history": jobs.history}


@app.post("/api/jobs/cancel")
async def job_cancel() -> dict:
    jobs.cancel()
    return {"ok": True}


@app.post("/api/recordings/start")
async def rec_start() -> dict:
    return {"name": hub.rec_start()}


@app.post("/api/recordings/stop")
async def rec_stop() -> dict:
    return {"name": hub.rec_stop()}


@app.get("/api/recordings")
def rec_list() -> list[dict]:
    if not REC_DIR.exists():
        return []
    return [{"name": p.name, "bytes": p.stat().st_size, "mtime": p.stat().st_mtime}
            for p in sorted(REC_DIR.glob("*.csv"), reverse=True)]


@app.get("/api/recordings/{name}")
def rec_get(name: str):
    p = (REC_DIR / name).resolve()
    if p.parent != REC_DIR.resolve() or not p.exists():
        raise HTTPException(404, "no such recording")
    return FileResponse(p, media_type="text/csv", filename=name)


@app.websocket("/ws")
async def ws(sock: WebSocket) -> None:
    await sock.accept()
    q: asyncio.Queue = asyncio.Queue()
    hub.clients.add(q)
    await sock.send_json(hub.snapshot())
    if jobs.job:
        await sock.send_json({"type": "job", "data": jobs.job})

    async def pump() -> None:
        while True:
            ev = await q.get()
            await sock.send_json(ev)

    sender = asyncio.create_task(pump())
    try:
        while True:
            msg = await sock.receive_json()
            t = msg.get("type")
            try:
                if t == "cmd":
                    _send(str(msg.get("line", "")))
                elif t == "stop":
                    hub.esc("UI stop")
                    jobs.cancel()
            except HTTPException as e:
                await sock.send_json({"type": "event", "level": "error", "text": e.detail})
    except WebSocketDisconnect:
        pass
    finally:
        sender.cancel()
        hub.clients.discard(q)


if DIST.exists():
    app.mount("/assets", StaticFiles(directory=DIST / "assets"), name="assets")

    @app.get("/{path:path}")
    def spa(path: str):
        f = DIST / path
        return FileResponse(f if path and f.is_file() else DIST / "index.html")
