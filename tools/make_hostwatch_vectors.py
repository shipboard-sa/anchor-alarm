"""
Purpose: Generate 'monitoring host down' vectors (ANC-25) for the C port's host test: scripted scenarios plus seeded
    random outage sequences are run through the Python reference (reference/anchorwatch/hostwatch.py); every
    observation and the expected event / state / lamp after it is written out for main/hostwatch.c to reproduce.
Version: v1 (2026-10-03)
Scope: STANDALONE UTILITY - anchor-box test tooling.
Architecture: reads reference/anchorwatch/hostwatch.py; writes test_host/vectors/hostwatch.txt (generated,
    gitignored), consumed by test_host/test_hostwatch.c. Line format:
      N name                                   new scenario (fresh watchers, default config)
      P t pi_ok router  event state asserted    Pi observation (router -1 = not checked, 0 silent, 1 answered)
      C t known last_cerbo last_other  event state asserted   Cerbo observation (-1 = never heard)
    event = name or '-'; state = number (hostwatch.py OK..NET_DOWN); asserted 0/1.
Invocation: reference\\.venv\\Scripts\\python tools\\make_hostwatch_vectors.py   (test_host\\run.bat runs it)
"""
from __future__ import annotations

import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "reference"))

from anchorwatch.hostwatch import CerboWatch, PiWatch   # noqa: E402


def pi_lines(seq):
    """seq: list of (t, pi_ok, router_up) - router_up is what a check WOULD return; checked only when asked"""
    w = PiWatch()
    out = []
    for t, ok, router_up in seq:
        r = (1 if router_up else 0) if w.wants_router(t) else -1
        ev = w.update(t, ok, None if r < 0 else bool(r))
        out.append(f"P {t} {int(ok)} {r} {ev[0] if ev else '-'} {w.state} {int(w.asserted)}")
    return out


def cerbo_lines(seq):
    """seq: list of (t, known, last_cerbo or -1, last_other or -1)"""
    w = CerboWatch()
    out = []
    for t, known, lc, lo in seq:
        ev = w.update(t, known, None if lc < 0 else lc, None if lo < 0 else lo)
        out.append(f"C {t} {int(known)} {lc} {lo} {ev[0] if ev else '-'} {w.state} {int(w.asserted)}")
    return out


def pi_random(seed, n=400):
    rnd = random.Random(seed)
    seq, t, ok, router = [], 0, True, True
    for _ in range(n):
        if rnd.random() < 0.08:
            ok = not ok
        if rnd.random() < 0.05:
            router = not router
        seq.append((t, ok, router))
        t += 10
    return seq


def cerbo_random(seed, n=3000):
    rnd = random.Random(seed)
    seq, lc, lo = [], -1, -1
    cerbo_on, bus_on, known = True, True, seed % 3 != 0
    for t in range(n):
        if rnd.random() < 0.004:
            cerbo_on = not cerbo_on
        if rnd.random() < 0.002:
            bus_on = not bus_on
        if not known and rnd.random() < 0.003:
            known = True
        if cerbo_on and bus_on and t % rnd.choice((1, 5, 60)) == 0:
            lc = t
        if bus_on:
            lo = t
        seq.append((t, known, lc, lo))
    return seq


def main():
    blocks = []
    blocks.append(("pi_healthy", pi_lines([(t, True, True) for t in range(0, 3600, 10)])))
    blocks.append(("pi_down", pi_lines([(t, False, True) for t in range(0, 300, 10)])))
    blocks.append(("pi_router_down", pi_lines([(t, False, t >= 300) for t in range(0, 500, 10)])))
    blocks.append(("pi_flap", pi_lines([(t, t >= 100 and t not in (120,), True) for t in range(0, 300, 10)])))
    for s in range(12):
        blocks.append((f"pi_random_{s}", pi_lines(pi_random(100 + s))))
    blocks.append(("cerbo_heartbeat", cerbo_lines([(t, True, t - t % 60, t) for t in range(0, 3600)])))
    blocks.append(("cerbo_silent", cerbo_lines([(t, True, min(t - t % 10, 90), t) for t in range(0, 600)])))
    blocks.append(("cerbo_bus_silent", cerbo_lines([(t, True, min(t - t % 10, 90), min(t, 99)) for t in range(0, 600)])))
    blocks.append(("cerbo_unknown", cerbo_lines([(t, False, -1, t) for t in range(0, 900)])))
    for s in range(12):
        blocks.append((f"cerbo_random_{s}", cerbo_lines(cerbo_random(200 + s))))
    outdir = os.path.join(ROOT, "test_host", "vectors")
    os.makedirs(outdir, exist_ok=True)
    lines = []
    for name, body in blocks:
        lines.append(f"N {name}")
        lines += body
    with open(os.path.join(outdir, "hostwatch.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    print(f"hostwatch vectors: {len(blocks)} scenarios, {len(lines) - len(blocks)} observations")


if __name__ == "__main__":
    main()
