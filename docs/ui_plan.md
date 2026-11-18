# Motor-control UI: plan

Goal: a React UI (shadcn) + Python backend that shows every input, every
measured value and every output of the board, runs every test, and drives the
motor live. Visual language: `DESIGN.md` (parchment, serif voice, one clay CTA).

```
browser (frontend/, Vite + React + shadcn)
   │  REST  /api/*        actions (connect, command, flash, run test, record)
   │  WS    /ws           live stream: telemetry, params, console, events, jobs
backend/ (FastAPI, pyserial)
   │  USB CDC  /dev/serial/by-id/usb-Custom_STM32G431_<app>_*-if00
firmware/ test_08_drive_lab  (+ "@" machine protocol)   ── or ──  simulator
```

## 1. Firmware (test_08_drive_lab + usb_console)

The human console stays exactly as it is. Machine lines start with `@` and
carry one JSON object, so the console stays readable and the backend parses
only what it needs.

| Line | When | Content |
|---|---|---|
| `@H {...}` | `hello` | app, protocol version, fs, CSA gain, pole-pair hint |
| `@T {...}` | every `tel <ms>` (0 = off) | state, mode, f/f_pll, amp applied/cap, I filt, per-phase instant/RMS/peak, KCL residual, flow %, id/iq + refs, vd/vq, λ est/use, duties A/B/C, θ, nFAULT, f max, I max, dropouts, stop reason, CSA offsets |
| `@P {...}` | after every command | every parameter (inputs) |
| `@R {...}` | run ended | kind, stop reason, f max, I max |
| `@RAW {...}` | `raw` done | duties, mean current per phase, flow % |
| `@RL {...}` | `rl` done | R, L, ok |
| `@C {...}` | `csa` / every start | offsets (V), per-phase OK, rebuilt phase |
| `@W {...}` + `@w` rows | `snap [n]` | waveform snapshot from the 20 kHz trace ring (live, not only after a stop) |

Fixes needed for a machine link:

* `usb_console.c`: 4-deep command queue (one pending line today, so a second
  quick command is silently dropped), and `echo off|on` handled in the console
  (echoed keystrokes could split a `@T` line).
* Idle telemetry: filtered live SOx current while idle (CSA noise/offset check).
* Telemetry keeps its own statistics window, so the human `log` lines are
  unchanged.

## 2. Backend (`backend/`, Python 3.12, FastAPI, uv)

* `link.py` – serial link thread (pyserial, DTR on), auto-connect to any
  `STM32G431_*` port, auto-reconnect after a flash, line parser.
* `sim.py` – simulated board speaking the same protocol (motor model with
  ramp, back-EMF, currents, faults) so the UI works with no hardware.
* `catalog.py` – test catalogue:
  * board apps `test_00`–`test_09`, `foc`: build, flash over DFU, capture the
    console, extract results with regexes (PASS/FAIL, offsets, verdict);
  * drive-lab procedures: raw duty table, open-loop 6-step/sine/SVPWM/DPWM
    sweeps, R/L, I/f, sensorless, custom spin (the `tools/lab.py` steps).
* `jobs.py` – one job at a time, streamed progress, cancel = ESC.
* Recording: telemetry to CSV in `backend/recordings/`, download from the UI.
* Safety: `/api/stop` sends ESC + `stop`; deadman: no UI client for 2 s while
  the bridge runs → ESC.

## 3. Frontend (`frontend/`, Vite + React + TS + Tailwind v4 + shadcn)

Component sourcing (check a library first, hand-build only what none has):

| Need | Source |
|---|---|
| buttons, cards, tabs, slider, select, switch, input, table, badge, tooltip, dialog, sheet, scroll-area, separator, toggle-group, collapsible, sonner, chart | shadcn/ui |
| animated numbers | Magic UI `number-ticker` |
| ring gauges | Magic UI `animated-circular-progress-bar` |
| staggered entrance | Magic UI `blur-fade` (motion) |
| live charts | shadcn `chart` (Recharts) |
| rotor angle / space-vector dial, duty bars, phase-current phasor | custom SVG + motion (no library has these) |

Pages:

1. **Drive** – mode (6-step, sine, SVPWM, DPWM, I/f, sensorless), start/stop,
   live sliders (amplitude, target frequency/rpm, current limit), ramp
   settings, hero readout (rpm, f, state), rotor dial, KPIs, charts (speed vs
   target, phase currents, dq currents), outputs (duties A/B/C, vd/vq).
2. **Scope** – snapshot waveforms (iA/iB/iC, duties, θ), auto-refresh.
3. **Tests** – catalogue cards with safety level, run, live log, results.
4. **Parameters** – every parameter with unit, limit and meaning, editable.
5. **Console** – raw terminal with history.

Global: connection/port picker, simulator switch, recording, and an
always-visible **STOP** (Esc / Space also stop).
