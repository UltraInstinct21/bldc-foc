"""Every test the UI can run.

Two kinds:
  app  – a board test application (firmware/tests/*, foc). Built, flashed over
         USB DFU, its console captured for `secs`, results pulled out with
         regexes. Expected outputs: firmware/tests/README.md.
  lab  – a procedure on the drive lab (test_08_drive_lab), the same steps
         tools/lab.py runs. Results come from the '@' protocol.

Lab steps:
  ("cmd", line)              send, wait for the @P acknowledgement
  ("csa",)                   measure CSA offsets, wait for @C
  ("raw", "a b c")           fixed duties for 300 ms, wait for @RAW
  ("rl",)                    R/L measurement, wait for @RL
  ("run", start, secs)       "start" | "if" | "sl"; wait for @R up to secs,
                             else ESC; then let the rotor coast down
  ("sleep", s)
"""
from __future__ import annotations

from typing import Any, Callable

RAW_TABLE = ["10 0 0", "0 10 0", "0 0 10", "10 10 0", "12 0 6", "26 14 20", "56 44 50"]

Input = dict[str, Any]


def inp(key: str, label: str, default: float, lo: float, hi: float, step: float, unit: str = "") -> Input:
    return {"key": key, "label": label, "default": default, "min": lo, "max": hi,
            "step": step, "unit": unit}


def _open_loop(mode: str) -> Callable[[dict], list]:
    def build(v: dict) -> list:
        return [("cmd", f"mode {mode}"), ("cmd", f"amp {v['amp']}"), ("cmd", f"f {v['f']}"),
                ("cmd", f"ilim {v['ilim']}"), ("run", "start", v["secs"])]
    return build


OPEN_INPUTS = [inp("amp", "Amplitude", 40, 5, 60, 1, "%"), inp("f", "Target frequency", 200, 5, 400, 5, "Hz"),
               inp("ilim", "Current limit", 2.0, 0.2, 4.5, 0.1, "A"), inp("secs", "Run time", 15, 3, 60, 1, "s")]

CATALOG: list[dict[str, Any]] = [
    # ---- board applications ---------------------------------------------------
    {"id": "test_00_hello", "kind": "app", "app": "test_00_hello", "group": "Board",
     "title": "Hello & clocks", "safety": "safe", "secs": 5,
     "covers": "3V3, 170 MHz clock, USB console, nSLEEP/DRVOFF/nFAULT idle levels",
     "metrics": [("SYSCLK", r"SYSCLK\s*:\s*(\d+)", "Hz")],
     "pass": r"SYSCLK\s*:\s*170000000", "fail": None},
    {"id": "test_01_as5600", "kind": "app", "app": "test_01_as5600", "group": "Board",
     "title": "AS5600 encoder", "safety": "safe", "secs": 6,
     "covers": "I2C1 on PA15/PB7, magnet status, raw angle (turn the shaft by hand)",
     "metrics": [("STATUS", r"STATUS=(0x[0-9A-Fa-f]+)", "")],
     "pass": r"AS5600 found", "fail": r"not answering|No answer"},
    {"id": "test_02_drv8316", "kind": "app", "app": "test_02_drv8316", "group": "Board",
     "title": "DRV8316 wake & DRVOFF", "safety": "safe", "secs": 6,
     "covers": "Driver wake, 30 µs fault-clear pulse, DRVOFF → nFAULT check",
     "metrics": [("nFAULT after wake", r"nFAULT low for (\d+)", "ms"), ("Init rc", r"rc=(-?\d+)", "")],
     "pass": r"DRVOFF check: PASS", "fail": r"DRVOFF check: FAIL|rc=-1"},
    {"id": "test_03_pwm", "kind": "app", "app": "test_03_pwm", "group": "Board",
     "title": "PWM pattern", "safety": "safe", "secs": 4,
     "covers": "TIM1 20 kHz, 3 complementary pairs, DPWMMIN, 1 µs dead time (check with a scope)",
     "metrics": [], "pass": None, "fail": None},
    {"id": "test_04_current_sense", "kind": "app", "app": "test_04_current_sense", "group": "Board",
     "title": "Current-sense offsets", "safety": "safe", "secs": 5,
     "covers": "ADC1 PA0/1/2 triggered by TIM1, zero-current offsets (window 1.3–2.0 V)",
     "metrics": [("SOA", r"SOA=([\d.]+)", "V"), ("SOB", r"SOB=([\d.]+)", "V"), ("SOC", r"SOC=([\d.]+)", "V")],
     "pass": r"SOA=1\.[3-9]\d*\s+V\s+SOB=1\.[3-9]\d*\s+V\s+SOC=1\.[3-9]", "fail": None},
    {"id": "test_05_open_loop", "kind": "app", "app": "test_05_open_loop", "group": "Board",
     "title": "Verified 6-step drive", "safety": "motor", "secs": 25,
     "covers": "Open-loop 6-step 4 → 200 Hz el. with current limiter, trip and retry",
     "metrics": [("CSA", r"CSA offsets .* -> (\w+)", "")],
     "pass": r"-> OK", "fail": r"trip|FAULT LOW"},
    {"id": "test_06_encoder_cal", "kind": "app", "app": "test_06_encoder_cal", "group": "Board",
     "title": "Encoder calibration", "safety": "twitch", "secs": 10,
     "covers": "Encoder sign/offset calibration, written to the last flash page",
     "metrics": [("rc", r"Encoder_Calibrate rc=(-?\d+)", ""), ("offset", r"offset\s*=\s*([\d.]+) rad", "rad")],
     "pass": r"rc=0", "fail": r"rc=-\d"},
    {"id": "test_07_esp_link", "kind": "app", "app": "test_07_esp_link", "group": "Board",
     "title": "ESP32 link", "safety": "safe", "secs": 6,
     "covers": "USART1 half-duplex 230400 to the ESP32 (needs the J7 short removed)",
     "metrics": [], "pass": r"ONLINE|link up", "fail": r"not started|refuse"},
    {"id": "test_09_drv_diag", "kind": "app", "app": "test_09_drv_diag", "group": "Board",
     "title": "DRV8316 analog diagnostics", "safety": "safe", "secs": 7,
     "covers": "3V3 rail, nFAULT voltage, SOx asleep/awake, wake time, DRVOFF response",
     "metrics": [("3V3 rail", r"VDDA\) = ([\d.]+)", "V"), ("nFAULT after wake", r"nFAULT low for (\d+) us", "µs"),
                 ("SOA awake", r"awake.*SOA ([\d.]+)", "V"), ("SOB awake", r"awake.*SOB ([\d.]+)", "V"),
                 ("SOC awake", r"awake.*SOC ([\d.]+)", "V"), ("Verdict", r"VERDICT: (.*)", "")],
     "pass": r"VERDICT: driver OK", "fail": r"VERDICT: (?!driver OK)"},
    {"id": "foc", "kind": "app", "app": "foc", "group": "Board",
     "title": "Sensored FOC", "safety": "motor", "secs": 15,
     "covers": "Closed-loop FOC on the AS5600 (needs the encoder wired and calibrated)",
     "metrics": [], "pass": None, "fail": r"not answering|rc=-"},

    # ---- drive-lab procedures ---------------------------------------------------
    {"id": "lab_csa", "kind": "lab", "group": "Drive lab", "title": "CSA offsets",
     "safety": "safe", "covers": "Zero-current offsets of SOA/SOB/SOC with the driver awake",
     "inputs": [], "build": lambda v: [("csa",)]},
    {"id": "lab_raw", "kind": "lab", "group": "Drive lab", "title": "Static duty table",
     "safety": "twitch", "covers": "Fixed duties for 300 ms each, mean current per phase: finds dead half-bridges and open SOx",
     "inputs": [], "build": lambda v: [("raw", r) for r in RAW_TABLE]},
    {"id": "lab_rl", "kind": "lab", "group": "Drive lab", "title": "Measure R and L",
     "safety": "twitch", "covers": "Phase resistance from two DC currents, inductance from 1/2 kHz injection",
     "inputs": [], "build": lambda v: [("rl",)]},
    {"id": "lab_6step", "kind": "lab", "group": "Drive lab", "title": "Open loop · 6-step",
     "safety": "motor", "covers": "Trapezoidal commutation ramp", "inputs": OPEN_INPUTS, "build": _open_loop("6step")},
    {"id": "lab_sine", "kind": "lab", "group": "Drive lab", "title": "Open loop · sine",
     "safety": "motor", "covers": "Sinusoidal modulation ramp", "inputs": OPEN_INPUTS, "build": _open_loop("sine")},
    {"id": "lab_svpwm", "kind": "lab", "group": "Drive lab", "title": "Open loop · SVPWM",
     "safety": "motor", "covers": "Space-vector modulation ramp", "inputs": OPEN_INPUTS, "build": _open_loop("svpwm")},
    {"id": "lab_dpwm", "kind": "lab", "group": "Drive lab", "title": "Open loop · DPWM",
     "safety": "motor", "covers": "Bottom-clamped discontinuous PWM ramp", "inputs": OPEN_INPUTS, "build": _open_loop("dpwm")},
    {"id": "lab_if", "kind": "lab", "group": "Drive lab", "title": "I/f current control",
     "safety": "motor", "covers": "dq current loop on SOx with an open-loop angle; estimates λ from back-EMF (needs R, L)",
     "inputs": [inp("f", "Target frequency", 60, 5, 200, 5, "Hz"), inp("ifa", "I/f current", 1.5, 0.1, 4, 0.1, "A"),
                inp("secs", "Run time", 15, 3, 60, 1, "s")],
     "build": lambda v: [("cmd", f"f {v['f']}"), ("cmd", f"set ifa {v['ifa']}"), ("run", "if", v["secs"])]},
    {"id": "lab_sl", "kind": "lab", "group": "Drive lab", "title": "Sensorless FOC",
     "safety": "motor", "covers": "I/f start, flux observer + PLL, handover, speed loop (needs R, L)",
     "inputs": [inp("f", "Speed target", 120, 10, 400, 5, "Hz"), inp("fsw", "Handover", 35, 5, 150, 1, "Hz"),
                inp("lam", "λ (0 = estimate)", 0.0009, 0, 0.01, 0.0001, "Wb"), inp("secs", "Run time", 25, 5, 90, 1, "s")],
     "build": lambda v: [("cmd", f"f {v['f']}"), ("cmd", f"set fsw {v['fsw']}"), ("cmd", f"set lam {v['lam']}"),
                         ("run", "sl", v["secs"])]},
    {"id": "lab_sweep", "kind": "lab", "group": "Drive lab", "title": "Modulation sweep",
     "safety": "motor", "covers": "6-step, sine, SVPWM and DPWM back to back with the same settings",
     "inputs": OPEN_INPUTS,
     "build": lambda v: sum((_open_loop(m)(v) for m in ("6step", "sine", "svpwm", "dpwm")), [])},
    {"id": "lab_characterise", "kind": "lab", "group": "Drive lab", "title": "Full characterisation",
     "safety": "motor", "covers": "CSA → duty table → R/L → I/f to 60 Hz: the bring-up sequence from HANDOFF.md",
     "inputs": [],
     "build": lambda v: [("csa",)] + [("raw", r) for r in RAW_TABLE] + [("rl",), ("cmd", "f 60"), ("run", "if", 15)]},
]


def public(t: dict) -> dict:
    """JSON-safe description for the UI."""
    out = {k: v for k, v in t.items() if k not in ("build", "metrics", "pass", "fail")}
    if t["kind"] == "app":
        out["metrics"] = [m[0] for m in t["metrics"]]
    else:
        out["steps"] = len(t["build"]({i["key"]: i["default"] for i in t.get("inputs", [])}))
    return out


def get(test_id: str) -> dict:
    for t in CATALOG:
        if t["id"] == test_id:
            return t
    raise KeyError(test_id)
