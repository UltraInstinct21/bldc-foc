#!/usr/bin/env python3
"""Replay the sensorless flux observer offline on a test_08_drive_lab trace.

    python3 tools/observer_replay.py ../docs/results/2026-11-18_if35_trace.txt \
        --f 35 --dec 2 --R 0.1378 --L 11.3e-6 --lam 0.0009 --vbus 9

The trace must come from a steady I/f run (rotor in sync), recorded with
`set trdec <dec>`, `if`, `stop`, `trace 512`. In I/f the true flux angle is the
open-loop angle minus a constant load angle, so a good voltage model gives the
true speed and a small angle wobble. This is how the 2026-11-18 session found
that the observer must use the written duties with NO dead-time subtraction.
"""
import argparse
import math

ap = argparse.ArgumentParser()
ap.add_argument("trace")
ap.add_argument("--f", type=float, required=True, help="true electrical Hz")
ap.add_argument("--dec", type=int, default=2, help="trace decimation (trdec)")
ap.add_argument("--R", type=float, default=0.1378)
ap.add_argument("--L", type=float, default=11.3e-6)
ap.add_argument("--lam", type=float, default=0.0009)
ap.add_argument("--vbus", type=float, default=9.0)
a = ap.parse_args()

rows = [[float(x) for x in l.split()[1:10]] for l in open(a.trace)
        if len(l.split()) >= 12 and l.split()[0].lstrip('-').isdigit()]
N = len(rows); dt = 50e-6 * a.dec; W = 2 * math.pi * a.f
I = [[r[0] / 1000, r[1] / 1000, r[2] / 1000] for r in rows]
D = [[r[3] / 1000, r[4] / 1000, r[5] / 1000] for r in rows]
TH = [r[6] / 3600 * 2 * math.pi for r in rows]
for k in range(N):                          # rebuild the highest-duty phase (KCL), as the firmware does
    mx = max(range(3), key=lambda j: D[k][j]); i = I[k]
    i[mx] = -(i[(mx + 1) % 3] + i[(mx + 2) % 3])
ab = lambda x: (x[0], (x[0] + 2 * x[1]) / math.sqrt(3))


def unwrap(v):
    o = [v[0]]
    for x in v[1:]:
        o.append(o[-1] + ((x - o[-1] + math.pi) % (2 * math.pi) - math.pi))
    return o


def run(dtsub=0.0, flt=False, R=a.R, lam=a.lam, scale=1.0):
    xa = lam * math.cos(TH[0]); xb = lam * math.sin(TH[0]); g = 2000 / lam ** 2; ang = []
    for k in range(1, N):
        ia, ib = ab(I[k]); pa = xa - a.L * ia; pb = xb - a.L * ib; i = I[k]; v = []
        for j in range(3):
            dd = D[k][j]
            if dd > 0:
                dd -= dtsub * max(-1, min(1, i[j] / 0.2))
            v.append(min(max(dd, 0), 1) * a.vbus * scale)
        fl = [j for j in range(3) if abs(i[j]) < 0.06]
        if flt and len(fl) == 1:            # floating phase: terminal voltage = its back-EMF
            f = fl[0]; ea = -W * pb; eb = W * pa
            e = [ea, -0.5 * ea + math.sqrt(3) / 2 * eb, -0.5 * ea - math.sqrt(3) / 2 * eb]
            p1, p2 = (f + 1) % 3, (f + 2) % 3; vN = 0.5 * (v[p1] + v[p2] + e[f])
            vn = [0.0] * 3; vn[p1] = v[p1] - vN; vn[p2] = v[p2] - vN; vn[f] = e[f]
        else:
            vN = sum(v) / 3; vn = [x - vN for x in v]
        va, vb = ab(vn); err = lam ** 2 - (pa * pa + pb * pb)
        xa += (va - R * ia + 0.5 * g * pa * err) * dt
        xb += (vb - R * ib + 0.5 * g * pb * err) * dt
        ang.append(math.atan2(xb - a.L * ib, xa - a.L * ia))
    u = unwrap(ang); t = unwrap(TH[1:]); df = [p - q for p, q in zip(u, t)][50:]
    m = sum(df) / len(df); sd = math.sqrt(sum((x - m) ** 2 for x in df) / len(df))
    return (u[-1] - u[50]) / ((N - 51) * dt) / (2 * math.pi), math.degrees(m) % 360, math.degrees(sd)


print(f"{N} samples, {N * dt * 1000:.0f} ms, true speed {a.f} Hz")
for name, kw in [("written duty x Vbus (firmware vrec 1)", {}),
                 ("+ floating-phase reconstruction (vrec 2)", {"flt": True}),
                 ("- full dead-time loss", {"dtsub": 0.02}),
                 ("R +30 %", {"R": a.R * 1.3}), ("R -30 %", {"R": a.R * 0.7}),
                 ("lambda +20 %", {"lam": a.lam * 1.2}), ("lambda -20 %", {"lam": a.lam * 0.8})]:
    f, m, s = run(**kw)
    print(f"{name:42s} speed {f:6.1f} Hz  lag {m:6.1f} deg  wobble {s:5.1f} deg")
