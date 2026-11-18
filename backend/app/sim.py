"""Simulated board: speaks test_08_drive_lab's console and '@' protocol.

Lets the UI be developed and demonstrated with no hardware. The motor model is
deliberately simple (first-order current, back-EMF, loss of sync above a
speed), with numbers taken from the bring-up results: 12N14P drone motor,
R = 0.138 ohm, L = 11 uH, lambda = 0.0009 Wb, 9 V supply. "Flashing" another
app makes the simulator print that app's documented console output instead.
"""
from __future__ import annotations

import json
import math
import random
import threading
import time
from typing import Optional

from .link import Link

TWO_PI = 2 * math.pi
MODS = ("6step", "sine", "svpwm", "dpwm")
R_TRUE, L_TRUE, LAM_TRUE = 0.138, 11.3e-6, 0.0009
F_SYNC_LOSS = 255.0                 # Hz el., where the printed motor lost sync

# Console output of the other apps, from firmware/tests/README.md and the
# bring-up logs (one block is printed per "period" seconds, repeated).
CANNED: dict[str, tuple[list[str], float]] = {
    "test_00_hello": ([
        "bldc_control_final_v6  |  test_00_hello",
        "SYSCLK     : 170000000 Hz",
        "HCLK       : 170000000 Hz",
        "USB        : CDC console up (HSI48 + CRS)",
    ], 0.0),
    "test_01_as5600": ([
        "=== test_01_as5600 ===",
        "AS5600 not answering at 0x36 (pull-ups? power? SDA on PB7?)",
    ], 2.0),
    "test_02_drv8316": ([
        "=== test_02_drv8316 (DRV8316T Hardware Mode) ===",
        "Straps: MODE=6x PWM, SLEW=125 V/us, GAIN=0.15 V/A, OCP/SR GND",
        "Idle: DRVOFF LOW, all INx low -> outputs Hi-Z.",
        "Waking DRV8316T (nSLEEP low -> high)...",
        "  nFAULT low for 2 ms after wake (expect ~2 ms)",
        "DRV8316_Init rc=0",
        "DRV8316T awake, nFAULT high.",
        "DRVOFF high: nFAULT=0 (expect 0)   DRVOFF low: nFAULT=1 (expect 1)",
        "DRVOFF check: PASS",
    ], 0.0),
    "test_03_pwm": ([
        "=== test_03_pwm ===",
        "TIM1 20 kHz centre-aligned, ARR 4249, dead time 1.0 us",
        "DPWMMIN 0.5 Hz electrical, DRV asleep (PWM_TEST_ENABLE_OUTPUT=0)",
    ], 0.0),
    "test_04_current_sense": ([
        "=== test_04_current_sense ===",
        "Offsets: SOA=1.6502 V  SOB=1.6519 V  SOC=1.6498 V",
        "  raw A/B/C        V A/B/C              I A/B/C (A)",
    ], 0.0),
    "test_05_open_loop": ([
        "=== test_05_open_loop (6-step, current limited) ===",
        "duty 40 %  ilim 2.0 A  f 4 -> 200 Hz el.  align 500 ms",
        "CSA offsets A 1.650 B 1.652 C 1.650 V -> OK",
        "   t_s  state  rpm    f_Hz  steps/s  duty%  I_A  (peak)  nFAULT",
    ], 0.0),
    "test_06_encoder_cal": ([
        "=== test_06_encoder_cal ===",
        "Encoder_Calibrate rc=-1 (AS5600 not answering)",
    ], 0.0),
    "test_07_esp_link": ([
        "=== test_07_esp_link ===",
        "J7 boot strap active (BOARD_J7_BOOT_STRAP=1): ESP32 link not started",
    ], 0.0),
    "test_09_drv_diag": ([
        "=== test_09_drv_diag ===  3V3 rail (VDDA) = 3.293 V",
        "  asleep (nSLEEP low):       nFAULT high (3.29 V) | SOA 0.001 SOB 0.002 SOC 0.001 V",
        "  awake 5 ms after wake:     nFAULT high (3.29 V) | SOA 1.655 SOB 1.654 SOC 1.655 V",
        "  nFAULT low for 1654 us after wake (healthy: ~1700-1800 us)",
        "  30 us nSLEEP pulse: nFAULT high (cleared / OK)",
        "  DRVOFF high: nFAULT LOW (expect LOW) | DRVOFF low: nFAULT high (expect high)",
        "VERDICT: driver OK. SOx mean 1.655 V (expect ~1.65 V = VREF/2)",
    ], 5.0),
    "foc": ([
        "=== foc ===",
        "AS5600 not answering: cannot run sensored FOC",
    ], 0.0),
    "foc_esp": ([
        "=== foc_esp ===",
        "AS5600 not answering: cannot run sensored FOC",
    ], 0.0),
}


def _f(x: float, n: int = 3) -> float:
    return round(float(x), n)


class SimLink(Link):
    kind = "sim"

    def __init__(self, on_line, on_close, app: str = "test_08_drive_lab"):
        super().__init__(on_line, on_close)
        self.port = "simulator"
        self.app = app
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._thread: Optional[threading.Thread] = None
        self._inbox: list[str] = []
        self._esc = False
        self.p = dict(vbus=9.0, ilim=2.0, amp=40.0, ampmax=60.0, f=200.0, fstart=4.0,
                      acc0=5.0, acc1=0.1, align=500, mode="6step", R=0.0, L=0.0,
                      lam=0.0, bw=400.0, ifa=1.5, fsw=35.0, kps=0.01, kis=0.05,
                      pll=30.0, idsl=-1.0, vrec=1, dtc=1, focmod="dpwm", trdec=1,
                      log=500, tel=0)
        self.echo = True
        self._reset_run()
        self.off = [1.6502, 1.6519, 1.6498]

    # ---- Link interface ------------------------------------------------------
    def start(self) -> None:
        self._thread = threading.Thread(target=self._run, name="sim", daemon=True)
        self._thread.start()

    def send(self, line: str) -> None:
        with self._lock:
            self._inbox.append(line)

    def esc(self) -> None:
        with self._lock:
            self._esc = True

    def close(self) -> None:
        self._stop.set()
        if self._thread and self._thread is not threading.current_thread():
            self._thread.join(timeout=1.0)

    @property
    def alive(self) -> bool:
        return self._thread is not None and not self._stop.is_set()

    def flash(self, app: str) -> None:
        """Pretend a new image was flashed: the console now belongs to `app`."""
        with self._lock:
            self.app = app
            self.p["tel"] = 0               # RAM settings do not survive a flash
            self.echo = True
            self._reset_run()
            self.st = "idle"
            self._boot_pending = True

    # ---- model state ---------------------------------------------------------
    def _reset_run(self) -> None:
        self.st = "idle"
        self.foc_kind = "if"
        self.obs = 0
        self.ms = 0.0
        self.theta = 0.0
        self.freq = 0.0
        self.fol = 0.0
        self.fpll = 0.0
        self.amp_app = 0.0
        self.cap = 2.0
        self.ifilt = 0.0
        self.ipk = 0.0
        self.id = self.iq = self.idr = self.iqr = self.vd = self.vq = 0.0
        self.lam_est = self.lam_use = 0.0
        self.fmax = self.imax = 0.0
        self.stop = getattr(self, "stop", "")
        self.inow = [0.0, 0.0, 0.0]
        self.duty = [0.0, 0.0, 0.0]
        self.step = -1
        self.rl_t = 0.0
        self.trace: list[list[int]] = []
        self._boot_pending = False

    def _out(self, s: str) -> None:
        self.on_line(s)

    def _json(self, tag: str, obj: dict) -> None:
        self._out(f"@{tag} " + json.dumps(obj, separators=(",", ":")))

    # ---- main loop -----------------------------------------------------------
    def _run(self) -> None:
        dt = 0.002
        last_tel = last_log = time.time()
        canned_t = 0.0
        self._out(f"[console] STM32G431 {self.app} (simulator). 'dfu' = bootloader, Ctrl-C/ESC = bridge off.")
        self._boot()
        while not self._stop.is_set():
            t0 = time.time()
            with self._lock:
                inbox, self._inbox = self._inbox, []
                esc, self._esc = self._esc, False
                boot = self._boot_pending
                self._boot_pending = False
            if boot:
                self._out(f"[console] STM32G431 {self.app} (simulator).")
                self._boot()
                canned_t = time.time()
            if esc:
                self._out("[stop] bridge off")
                self._bridge_stop("ESC")
            for line in inbox:
                self._command(line.strip())
            if self.app == "test_08_drive_lab":
                self._step(dt)
                now = time.time()
                if self.p["tel"] and now - last_tel >= self.p["tel"] / 1000.0:
                    last_tel = now
                    self._tel()
                if self.st in ("align", "run", "foc") and now - last_log >= self.p["log"] / 1000.0:
                    last_log = now
                    self._log_line()
            else:
                lines, period = CANNED.get(self.app, ([f"=== {self.app} ==="], 0.0))
                if period and time.time() - canned_t >= period:
                    canned_t = time.time()
                    for ln in lines[1:]:
                        self._out(ln)
            time.sleep(max(0.0, dt - (time.time() - t0)))

    def _boot(self) -> None:
        if self.app == "test_08_drive_lab":
            self._out("=== test_08_drive_lab ===")
            self._params_human()
            self._out("ready")
        else:
            for ln in CANNED.get(self.app, ([f"=== {self.app} ==="], 0.0))[0]:
                self._out(ln)

    # ---- motor model ---------------------------------------------------------
    def _bridge_stop(self, why: str) -> None:
        if self.st != "idle":
            prev = self.st
            self.stop = why
            self.st = "idle"
            self.duty = [0.0, 0.0, 0.0]
            self.step = -1
            if prev in ("align", "run", "foc"):
                kind = self.foc_kind if prev == "foc" else self.p["mode"]
                label = {"if": "I/f", "sl": "sensorless"}.get(kind, kind)
                self._out(f"RESULT {label}: stopped ({why}), max f {self.fmax:.1f} Hz, "
                          f"max |I| sample {self.imax:.2f} A")
                self._json("R", {"kind": kind, "why": why, "fmax": _f(self.fmax, 2),
                                 "imax": _f(self.imax)})
                self._out("ready")

    def _slew(self, x: float, t: float, step: float) -> float:
        return min(x + step, t) if x < t else max(x - step, t)

    def _phase_currents(self, mag: float, ang: float) -> list[float]:
        n = 0.04
        return [mag * math.cos(ang) + random.gauss(0, n),
                mag * math.cos(ang - TWO_PI / 3) + random.gauss(0, n),
                mag * math.cos(ang + TWO_PI / 3) + random.gauss(0, n)]

    def _step(self, dt: float) -> None:
        p = self.p
        if self.st == "idle":
            self.inow = [random.gauss(0, 0.01) for _ in range(3)]
            return
        self.ms += dt * 1000
        if self.st in ("align", "run"):
            self.amp_app = self._slew(self.amp_app, p["amp"], 50 * dt)
            self.amp_app = min(self.amp_app, self.cap)
            if self.ifilt > p["ilim"]:
                self.cap = max(0.0, min(self.cap, self.amp_app) - 100 * dt)
            else:
                self.cap = min(self.cap + 20 * dt, self.amp_app + 2.0)
            if self.st == "align":
                if self.ms >= p["align"]:
                    self.st, self.freq = "run", p["fstart"]
            else:
                self.freq = self._slew(self.freq, p["f"], (p["acc0"] + p["acc1"] * self.freq) * dt)
            w = TWO_PI * self.freq
            vph = self.amp_app / 100 * p["vbus"] * (0.5 if self.p["mode"] == "6step" else 0.577)
            bemf = LAM_TRUE * w
            imag = max(0.15, (vph - 0.92 * bemf)) / (R_TRUE * 5.5) + 0.25 * (self.freq / 200) ** 2
            self.theta = (self.theta + w * dt) % TWO_PI
            ang = self.theta
            if p["mode"] == "6step":
                self.step = min(5, int(self.theta / (math.pi / 3)))
            if self.freq > F_SYNC_LOSS:
                imag *= 3.0
            self.ifilt += 0.25 * (imag - self.ifilt)
            self.inow = self._phase_currents(imag, ang - 0.4)
            m = self.amp_app / 100 / math.sqrt(3)
            v = [m * math.cos(ang), m * math.cos(ang - TWO_PI / 3), m * math.cos(ang + TWO_PI / 3)]
            self.duty = self._modulate(p["mode"], v)
            self.fmax = max(self.fmax, self.freq)
            if self.ifilt > min(2 * p["ilim"], 5.3):
                self._bridge_stop("overcurrent")
        elif self.st == "foc":
            iref = min(p["ifa"], p["ilim"])
            if self.ms <= 200:
                self.idr = iref * self.ms / 200
            elif self.obs == 0 and self.ms > p["align"] + 200:
                tgt = p["f"] if self.foc_kind == "if" else p["fsw"]
                self.fol = self._slew(self.fol, tgt, (p["acc0"] + p["acc1"] * self.fol) * dt)
            if self.foc_kind == "sl":
                if self.obs == 0 and self.fol >= 0.8 * p["fsw"]:
                    self.lam_use = p["lam"] if p["lam"] > 0 else self.lam_est
                if self.obs == 0 and self.fol >= p["fsw"] and self.lam_use > 0:
                    self.obs = 1
                    self._out(f"handover at {self.fol:.1f} Hz")
                if self.obs:
                    self.fol = self._slew(self.fol, p["f"], (p["acc0"] + p["acc1"] * self.fol) * dt)
                    self.idr = self._slew(self.idr, p["idsl"], 4 * dt)
                    self.iqr = max(-p["ilim"], min(p["ilim"], 0.3 + 0.004 * self.fol))
                self.fpll = self.fol + (random.gauss(0, 0.3) if self.obs or self.fol > 0.7 * p["fsw"] else 0)
                if self.obs and self.fol > 70:
                    self._bridge_stop("observer lost lock (speed collapsed)")
                    return
            w = TWO_PI * self.fol
            self.id = self.idr + random.gauss(0, 0.05)
            self.iq = self.iqr + random.gauss(0, 0.05)
            self.vd = R_TRUE * self.id - w * L_TRUE * self.iq
            self.vq = R_TRUE * self.iq + w * L_TRUE * self.id + (w * LAM_TRUE if self.obs else 0)
            if self.fol > 10 and self.obs == 0:
                self.lam_est += 0.01 * (LAM_TRUE * (1 + random.gauss(0, 0.03)) - self.lam_est)
            self.theta = (self.theta + w * dt) % TWO_PI
            ang = self.theta + math.atan2(self.iq, self.id)
            mag = math.hypot(self.id, self.iq)
            self.inow = self._phase_currents(mag, ang)
            self.ifilt += 0.2 * (mag - self.ifilt)
            vm = math.hypot(self.vd, self.vq) / p["vbus"]
            v = [vm * math.cos(ang), vm * math.cos(ang - TWO_PI / 3), vm * math.cos(ang + TWO_PI / 3)]
            self.duty = self._modulate(p["focmod"], v)
            self.fmax = max(self.fmax, self.fol)
        elif self.st == "rl":
            self.rl_t += dt
            self.inow = self._phase_currents(1.0 if self.rl_t < 0.3 else 2.0, 0.0)
            self.duty = [0.04, 0.0, 0.0]
            if self.rl_t >= 1.1:
                self.st = "idle"
                self.stop = "rl done"
                ls = [L_TRUE * 1e6 * (1 + random.gauss(0, 0.04)) for _ in range(2)]
                r = R_TRUE * (1 + random.gauss(0, 0.01))
                self._out(f"RL: vd 0.141 V @ 1.00 A, 0.279 V @ 2.00 A -> R = {r:.4f} ohm (phase, Vbus {p['vbus']:.1f} V)")
                self._out(f"RL: |Ih| 3.521 A @1kHz, 1.761 A @2kHz -> L = {ls[0]:.1f} / {ls[1]:.1f} uH")
                p["R"], p["L"] = r, sum(ls) / 2
                self._out(f"RL: using R = {r:.4f} ohm, L = {p['L']:.1f} uH")
                self._json("RL", {"ok": 1, "R": _f(r, 5), "L1k": _f(ls[0], 2), "L2k": _f(ls[1], 2),
                                  "L": _f(p["L"], 2), "v": [0.1410, 0.2790], "i": [1.0, 2.0]})
                self._out("ready")
        self.imax = max(self.imax, max(abs(x) for x in self.inow))
        self._record()

    def _modulate(self, mod: str, v: list[float]) -> list[float]:
        if mod == "6step":
            d = self.amp_app / 100
            pat = [("P", "L", "Z"), ("P", "Z", "L"), ("Z", "P", "L"),
                   ("L", "P", "Z"), ("L", "Z", "P"), ("Z", "L", "P")][max(0, self.step)]
            return [d if s == "P" else 0.0 for s in pat]
        vmax, vmin = max(v), min(v)
        if mod == "sine":
            out = [0.5 + x for x in v]
        elif mod == "svpwm":
            off = 0.5 - 0.5 * (vmax + vmin)
            out = [x + off for x in v]
        else:
            out = [x - vmin for x in v]
        return [max(0.0, min(0.98, x)) for x in out]

    def _record(self) -> None:
        # one trace row per sim step; the snapshot interpolates to 50 us steps
        th = math.degrees(self.theta)
        self.trace.append([int(x * 1000) for x in self.inow] + [int(d * 1000) for d in self.duty]
                          + [int(th * 10) % 3600, int(th * 10) % 3600, 7, 0])
        if len(self.trace) > 512:
            del self.trace[:len(self.trace) - 512]

    # ---- output --------------------------------------------------------------
    def _tel(self) -> None:
        mags = [abs(x) for x in self.inow]
        rms = [self.ifilt / math.sqrt(2)] * 3 if self.st != "idle" else [0.0] * 3
        th = math.degrees(self.theta)
        self._json("T", {
            "t": int(time.time() * 1000) & 0x7FFFFFFF, "st": self.st, "mod": self.p["mode"],
            "kind": self.foc_kind, "obs": self.obs, "ms": int(self.ms),
            "f": _f(self.freq, 2), "fol": _f(self.fol, 2), "fpll": _f(self.fpll, 2),
            "amp": _f(self.amp_app, 2), "cap": _f(self.cap, 2), "ifilt": _f(self.ifilt),
            "i": [_f(x) for x in self.inow],
            "iidle": [_f(random.gauss(0, 0.005)) for _ in range(3)],
            "rms": [_f(x * (1 + random.gauss(0, 0.02))) for x in rms],
            "pk": [_f(max(m, r * 1.45)) for m, r in zip(mags, rms)],
            "res": _f(abs(random.gauss(0, 0.04)) if self.st not in ("idle",) and self.p["mode"] != "6step" else -1.0),
            "flow": 0 if self.st == "idle" else min(100, int(80 + 20 * min(1, self.ifilt))),
            "drop": 0, "id": _f(self.id), "iq": _f(self.iq), "idr": _f(self.idr), "iqr": _f(self.iqr),
            "vd": _f(self.vd), "vq": _f(self.vq), "lam": _f(self.lam_est, 6), "lamu": _f(self.lam_use, 6),
            "d": [_f(x, 4) for x in self.duty], "step": self.step, "th": _f(th, 1), "tho": _f(th, 1),
            "nf": 0, "fmax": _f(self.fmax, 2), "imax": _f(self.imax),
            "off": self.off, "bad": -1, "stop": self.stop,
        })

    def _log_line(self) -> None:
        if self.st == "foc":
            self._out(f"{time.time() % 1000:7.2f} {'SL ' if self.obs else 'I/f'} f_ol {self.fol:6.1f} "
                      f"f_pll {self.fpll:6.1f} | id {self.id:+5.2f} iq {self.iq:+5.2f} | ok")
        else:
            self._out(f"{time.time() % 1000:7.2f} {self.p['mode']:<5} {self.st.upper():<5} f {self.freq:6.1f} "
                      f"amp {self.amp_app:5.1f}% | I {self.ifilt:4.2f} | ok")

    def _params(self) -> None:
        p = self.p
        self._json("P", {k: (_f(v, 6) if isinstance(v, float) else v) for k, v in p.items()})

    def _params_human(self) -> None:
        p = self.p
        self._out(f"vbus {p['vbus']:.1f} V ilim {p['ilim']:.2f} A amp {p['amp']:.1f}% (max {p['ampmax']:.0f}%) "
                  f"f {p['f']:.1f} Hz fstart {p['fstart']:.1f} accel {p['acc0']:.1f}+{p['acc1']:.2f}*f align {p['align']} ms")
        self._out(f"R {p['R']:.4f} ohm L {p['L']:.1f} uH lam {p['lam']:.5f} bw {p['bw']:.0f} Hz ifa {p['ifa']:.2f} A "
                  f"fsw {p['fsw']:.1f} Hz kps {p['kps']:.3f} kis {p['kis']:.3f} dtc {p['dtc']} focmod {p['focmod']} mode {p['mode']}")

    def _csa(self) -> None:
        self.off = [_f(1.65 + random.gauss(0, 0.002), 4) for _ in range(3)]
        self._json("C", {"off": self.off, "ok": [1, 1, 1], "bad": -1, "valid": 1})

    def _snap(self, n: int) -> None:
        rows = self.trace[-n:] if self.st != "idle" else self.trace[-n:]
        # expand 2 ms sim steps into 50 us rows with a synthetic waveform
        n = min(n or 256, 512)
        out = []
        if self.st != "idle":
            w = TWO_PI * (self.freq if self.st in ("align", "run") else self.fol)
            mag = max(self.ifilt, 0.05)
            for k in range(n):
                ang = self.theta - w * (n - k) * 50e-6 - 0.4
                i = self._phase_currents(mag, ang)
                m = self.amp_app / 100 / math.sqrt(3) if self.st != "foc" else math.hypot(self.vd, self.vq) / self.p["vbus"]
                v = [m * math.cos(ang + 0.4), m * math.cos(ang + 0.4 - TWO_PI / 3), m * math.cos(ang + 0.4 + TWO_PI / 3)]
                saved = self.step
                if self.p["mode"] == "6step" and self.st != "foc":
                    self.step = int(((ang + 0.4) % TWO_PI) / (math.pi / 3)) % 6
                    i = [x if pat != "Z" else 0.0 for x, pat in zip(i, [("P", "L", "Z"), ("P", "Z", "L"), ("Z", "P", "L"), ("L", "P", "Z"), ("L", "Z", "P"), ("Z", "L", "P")][self.step])]
                d = self._modulate(self.p["mode"] if self.st != "foc" else self.p["focmod"], v)
                self.step = saved
                th = math.degrees((ang + 0.4) % TWO_PI)
                out.append([int(x * 1000) for x in i] + [int(x * 1000) for x in d] + [int(th * 10), int(th * 10), 7, 0])
        else:
            out = rows
        self._json("W", {"n": len(out), "dt_us": 50, "st": self.st, "frozen": 1 if self.st == "idle" else 0})
        for r in out:
            self._out("@w " + " ".join(str(x) for x in r))
        self._json("W", {"end": 1})

    # ---- commands ------------------------------------------------------------
    def _command(self, line: str) -> None:
        if not line:
            return
        if line in ("echo off", "echo on"):
            self.echo = line == "echo on"
            return
        if line == "dfu":
            self._out("Rebooting into the ST DFU bootloader...")
            return
        if self.echo:
            self._out(line)
        if self.app != "test_08_drive_lab":
            return
        a = line.split()
        c, p = a[0], self.p

        def num(k: int, default: float = 0.0) -> float:
            try:
                return float(a[k])
            except (IndexError, ValueError):
                return default

        def clamp(x, lo, hi):
            return max(lo, min(hi, x))

        if c == "help":
            self._out("commands: mode start stop raw rl if sl amp f ilim vbus accel align fstart set trace log tel hello csa snap")
        elif c == "params":
            self._params_human()
        elif c == "stop":
            self._bridge_stop("stop command")
        elif c == "start":
            self._start_prepare()
            self.st = "align"
            self._out(f"start {p['mode']}: amp {p['amp']:.0f}% f {p['f']:.0f} Hz ilim {p['ilim']:.1f} A")
        elif c == "raw" and len(a) == 4:
            d = [num(1), num(2), num(3)]
            if max(d) - min(d) > 20:
                self._out("raw: spread limited to 20%")
            else:
                self._start_prepare()
                mean = sum(d) / 3
                cur = [clamp((x - mean) / 100 * p["vbus"] / (R_TRUE * 1.5) / 6.5, -5, 5) for x in d]
                self._out(f"RAW {d[0]:5.1f} {d[1]:5.1f} {d[2]:5.1f} | mean A {cur[0]:+6.2f} B {cur[1]:+6.2f} C {cur[2]:+6.2f} A")
                self._json("RAW", {"d": d, "mean": [_f(x) for x in cur],
                                   "flow": [100 if abs(x) > 0.1 else 0 for x in cur], "why": "raw done"})
                self.stop = "raw done"
            self._out("ready")
        elif c == "rl":
            self._start_prepare()
            self.st, self.rl_t = "rl", 0.0
            self._out("rl: holding 1 A then 2.0 A at theta=0, then 0.25 V injection at 1/2 kHz...")
        elif c in ("if", "sl"):
            if p["R"] <= 0 or p["L"] <= 0:
                self._out("run 'rl' first (or set R / set L)")
            else:
                self._start_prepare()
                self.st, self.foc_kind, self.obs = "foc", c, 0
                self._out(f"start {'I/f' if c == 'if' else 'sensorless'}: I {min(p['ifa'], p['ilim']):.2f} A, "
                          f"R {p['R']:.4f} ohm, L {p['L']:.1f} uH, mod {p['focmod']}, dtc {p['dtc']}")
        elif c == "mode" and len(a) == 2:
            if self.st != "idle":
                self._out("stop first")
            elif a[1] not in MODS:
                self._out("unknown mode")
            else:
                p["mode"] = a[1]
                self._out(f"mode {a[1]}")
        elif c == "amp":
            p["amp"] = clamp(num(1), 0, p["ampmax"])
        elif c == "f":
            p["f"] = clamp(num(1), 0.05, 400)
        elif c == "ilim":
            p["ilim"] = clamp(num(1), 0.2, 4.5)
        elif c == "vbus":
            p["vbus"] = clamp(num(1), 4.5, 30)
        elif c == "fstart":
            p["fstart"] = clamp(num(1), 0.05, 50)
        elif c == "align":
            p["align"] = int(clamp(num(1), 0, 5000))
        elif c == "accel":
            p["acc0"], p["acc1"] = num(1), num(2)
        elif c == "log":
            p["log"] = int(clamp(num(1), 50, 10000))
        elif c == "tel":
            v = num(1)
            p["tel"] = 0 if v <= 0 else int(clamp(v, 20, 10000))
        elif c == "hello":
            self._json("H", {"app": "test_08_drive_lab", "proto": 1, "fs": 20000, "csa": 0.15,
                             "itrip": 5.3, "trn": 512, "built": "simulator", "sim": 1})
        elif c == "csa":
            if self.st != "idle":
                self._out("stop first")
            else:
                self._csa()
        elif c == "snap":
            self._snap(int(num(1, 256)))
        elif c == "trace":
            self._out("TRACE (simulator): use snap")
        elif c == "set" and len(a) == 3:
            k, v = a[1], num(2)
            table = {
                "R": lambda: v, "L": lambda: v, "lam": lambda: v, "bw": lambda: clamp(v, 50, 2000),
                "ifa": lambda: clamp(v, 0.1, 4), "fsw": lambda: clamp(v, 5, 300), "kps": lambda: v,
                "kis": lambda: v, "dtc": lambda: 1 if v else 0, "pll": lambda: clamp(v, 5, 200),
                "ampmax": lambda: clamp(v, 10, 95), "idsl": lambda: clamp(v, -3, 0),
                "vrec": lambda: int(clamp(v, 0, 2)), "trdec": lambda: int(clamp(v, 1, 100)),
            }
            if k == "focmod":
                if a[2] in ("sine", "svpwm", "dpwm"):
                    p["focmod"] = a[2]
                else:
                    self._out("focmod sine|svpwm|dpwm")
            elif k in table:
                p[k] = table[k]()
                if k == "ampmax":
                    p["amp"] = min(p["amp"], p["ampmax"])
            else:
                self._out("unknown parameter")
        else:
            self._out("? (help)")
        self._params()
        if self.st == "idle":
            self._out("ok")

    def _start_prepare(self) -> None:
        stop_keep = self.stop
        self._reset_run()
        self.stop = ""
        del stop_keep
        self._csa()
