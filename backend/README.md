# Backend: board ↔ UI bridge

FastAPI + pyserial. Talks to `test_08_drive_lab` over its USB CDC console,
parses the firmware's `@` machine protocol, and streams it to the React UI over
a WebSocket. Also builds/flashes the board test apps and runs the test
catalogue. Plan and protocol: [`../docs/ui_plan.md`](../docs/ui_plan.md).

```sh
uv sync
uv run uvicorn app.main:app --port 8000            # board over USB (auto-connect)
BLDC_SIM=1 uv run uvicorn app.main:app --port 8000 # simulator, no hardware
```

If `../frontend/dist` exists it is served at `/` (`../run_ui.sh` builds it).

| File | Role |
|---|---|
| `app/link.py` | Serial link (DTR held, reader thread), port discovery |
| `app/sim.py` | Simulated board speaking the same console + `@` protocol |
| `app/protocol.py` | Line parser (`@T`, `@P`, `@R`, `@RAW`, `@RL`, `@C`, `@W`/`@w`, `@H`) |
| `app/hub.py` | Connection supervisor, live state, WebSocket fan-out, CSV recording, deadman stop |
| `app/catalog.py` | Every test: board apps (flash + capture + regex results) and drive-lab procedures |
| `app/jobs.py` | Runs one test at a time, streams progress and results |
| `app/main.py` | REST + `/ws` |

## API

| Method | Path | |
|---|---|---|
| GET | `/api/status` | connection, port, running app, recording, current job |
| POST | `/api/connect` | `{"mode": "auto" \| "serial" \| "sim" \| "off", "port": "..."}` |
| POST | `/api/command` | `{"line": "amp 30"}`; only allow-listed drive-lab commands |
| POST | `/api/stop` | ESC + `stop`, cancels a running test |
| GET | `/api/tests` | test catalogue |
| POST | `/api/tests/{id}/run` | `{"values": {...}}` |
| POST | `/api/flash` | `{"app": "test_08_drive_lab"}` |
| POST | `/api/jobs/cancel` | |
| POST | `/api/recordings/start`, `/stop`; GET `/api/recordings[/{name}]` | telemetry CSV |
| WS | `/ws` | server → client: `snapshot`, `status`, `tel`, `params`, `hello`, `csa`, `wave`, `result`, `raw`, `rl`, `console`, `sent`, `event`, `job`, `job_log`. Client → server: `{"type":"cmd","line":...}`, `{"type":"stop"}` |

## Safety

- STOP in the UI, Esc or Space sends ESC (the firmware turns the bridge off in
  the USB interrupt path) and then `stop`.
- Deadman: if no UI is connected for 2 s while the bridge runs, the backend
  stops it.
- Commands are allow-listed (`ALLOWED` in `app/main.py`); firmware limits
  (amp ≤ ampmax, ilim ≤ 4.5 A, trip 5.3 A) still apply on the board.
- Closing the port (backend exit) drops DTR; ESC is sent first if running.
