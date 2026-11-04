"""
Purpose: Generate anchor-watch scenario vectors for the C port's host test: simulated anchorages (sim.py) are run
    through the Python reference watch (watch.py); every command and every expected event is written out so the
    C watch (main/anchorwatch.c) must reproduce the same events at the same seconds and the same final circle.
Version: v2 (2026-10-02) - v2: power-cut restart scenarios (R command)
Scope: STANDALONE UTILITY - anchor-box test tooling.
Architecture: reads reference/anchorwatch (sim + watch); writes test_host/vectors/*.txt (generated, gitignored),
    consumed by test_host/test_watch.c. Line format:
      A t rode_ft          arm            K t        tick (1 Hz, like the firmware)
      U t lat lon          position fix   D t        disarm
      R t_off t_on         power cut: snapshot at t_off, a fresh watch restored at t_on (no fixes between)
      E t kind state       an event the PREVIOUS command must produce (kind 0 STATE, 1 WARN, 2 DRAG, 3 INFO)
      F state has_circle lat lon alarm_radius     final status
Invocation: reference\\.venv\\Scripts\\python tools\\make_watch_vectors.py   (test_host\\run.bat runs it)
"""
from __future__ import annotations

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "reference"))

from anchorwatch import sim                             # noqa: E402
from anchorwatch.watch import AnchorWatch, WatchConfig   # noqa: E402

KINDS = {"STATE": 0, "WARN": 1, "DRAG": 2, "INFO": 3}
STATES = {"OFF": 0, "ACQUIRING": 1, "LOCKED": 2, "CONSERVATIVE": 3, "ALARM": 4}


def scenarios():
    S = sim.simulate
    yield "steady", 100, S(duration_s=3600, swing="steady", seed=1), {}
    yield "large", 150, S(duration_s=3600, swing="large", radius_m=60, seed=2), {}
    yield "small", 100, S(duration_s=3600, swing="small", seed=3), {}
    yield "none", 100, S(duration_s=3600, swing="none", seed=4), {}
    yield "reversal", 100, S(duration_s=5400, swing="reversal", seed=5), {}
    yield "small_reversal", 100, S(duration_s=5400, swing="small-reversal", seed=6), {}
    yield "slow_drag", 100, S(duration_s=7200, drag=sim.Drag("slow", 3600, 0.03), seed=7), {}
    yield "sudden_drag", 100, S(duration_s=5400, drag=sim.Drag("sudden", 3600), seed=8), {}
    yield "stretch", 100, S(duration_s=7200, stretch=sim.Stretch(3600, 8.0), seed=9), {}
    yield "outliers", 100, S(duration_s=3600, outlier_prob=0.02, seed=10), {}
    yield "wander_4h", 100, S(duration_s=14400, walk_sigma_m=3.0, seed=11), {}
    yield "coarse_guard", 100, S(duration_s=1200, drag=sim.Drag("slow", 120, 0.5), seed=12), {}
    yield "no_rode", 0, S(duration_s=3600, swing="small", seed=13), {}
    yield "gps_gap", 100, S(duration_s=3600, seed=14), {"gap": (1500, 1600)}
    yield "dup_repeat", 100, S(duration_s=2400, seed=15), {"dups": True}
    yield "rearm", 100, S(duration_s=4800, seed=16), {"rearm_at": 2000, "disarm_at": 4000}
    yield "lock_restart", 100, S(duration_s=3600, seed=21), {"restart": (2400, 3000)}
    yield "drag_restart", 100, S(duration_s=7200, drag=sim.Drag("slow", 2500, 0.05), seed=22), {"restart": (2400, 5000)}
    yield "alarm_restart", 100, S(duration_s=5400, drag=sim.Drag("sudden", 3000), seed=23), {"restart": (4000, 4100)}
    yield "cons_restart", 100, S(duration_s=3600, swing="none", seed=24), {"restart": (1200, 1300)}


def run(name, rode, fixes, opts, outdir):
    w = AnchorWatch(WatchConfig())
    lines = []

    def emit(cmd, evs):
        lines.append(cmd)
        for e in evs:
            lines.append(f"E {e.t!r} {KINDS[e.kind]} {STATES[e.state]}")

    t0 = fixes[0].t
    emit(f"A {t0!r} {rode!r}", w.arm(t0, rode_ft=rode or None))
    gap = opts.get("gap")
    restart = opts.get("restart")
    for i, f in enumerate(fixes):
        t = f.t
        if restart and t == restart[0]:
            snap = w.snapshot()
            assert snap is not None, name
            w = AnchorWatch(WatchConfig())
            emit(f"R {restart[0]!r} {restart[1]!r}", w.restore(restart[1], snap))
        if restart and restart[0] <= t < restart[1]:
            continue
        if opts.get("rearm_at") == t:
            emit(f"A {t!r} {rode!r}", w.arm(t, rode_ft=rode or None))
        if opts.get("disarm_at") == t:
            emit(f"D {t!r}", w.disarm(t))
        if not (gap and gap[0] <= t - t0 < gap[1]):
            emit(f"U {t!r} {f.lat!r} {f.lon!r}", w.update(t, f.lat, f.lon))
            if opts.get("dups") and i % 97 == 0:
                emit(f"U {t!r} {f.lat!r} {f.lon!r}", w.update(t, f.lat, f.lon))          # duplicate time
            if opts.get("dups") and i % 89 == 0:
                emit(f"U {t + 0.5!r} {f.lat!r} {f.lon!r}", w.update(t + 0.5, f.lat, f.lon))  # repeated position
        emit(f"K {t + 0.9!r}", w.tick(t + 0.9))
    st = w.status()
    has = st.centre_lat is not None and w.centre is not None
    lines.append(f"F {STATES[st.state]} {int(has)} {st.centre_lat if has else 0.0!r} {st.centre_lon if has else 0.0!r} "
                 f"{st.alarm_radius_m if st.alarm_radius_m is not None else 0.0!r}")
    path = os.path.join(outdir, f"{name}.txt")
    with open(path, "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(lines) + "\n")
    nev = sum(1 for ln in lines if ln.startswith("E "))
    print(f"{name:15s} final {st.state:12s} events {nev:3d}")


def main() -> None:
    outdir = os.path.join(ROOT, "test_host", "vectors")
    os.makedirs(outdir, exist_ok=True)
    names = []
    for name, rode, fixes, opts in scenarios():
        run(name, rode, fixes, opts, outdir)
        names.append(name)
    with open(os.path.join(outdir, "index.txt"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write("\n".join(names) + "\n")


if __name__ == "__main__":
    main()
