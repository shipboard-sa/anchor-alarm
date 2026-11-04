"""
Purpose: Command line for the anchor watch - `replay` a recorded track, `sim` a synthetic swing to GPX,
    `run` the DRY-RUN live watch against a (loopback) SignalK.
Version: v3 (2026-10-02) - v3: --rode-ft on replay + run (rode pick, 50 ft steps). v2 (2026-10-01) - v2: run --follow-mode (D-092). v1 (2026-09-30)
Scope: STANDALONE UTILITY - anchor-watch CLI (dry run only; drives nothing).
Architecture: anchor-watch entry point; dispatches to replay.py, sim.py, service.py.
Invocation: from reference, with its .venv (or any Python 3.10+; `run` needs
    `websockets` unless --poll):
    python -m anchorwatch replay TRACK.gpx|.csv [--arm-at SECONDS|ISO] [--rode-ft 150] [--tolerance 15] [--debounce 20] [--every 600]
    python -m anchorwatch sim --out swing.gpx [--pattern steady|large|small|none|reversal|small-reversal]
                              [--hours 2] [--radius 40] [--drag slow|sudden] [--drag-at 3600] [--every 1] [--seed 1]
    python -m anchorwatch run [--signalk http://127.0.0.1:3100] [--arm | --follow-mode] [--rode-ft 150] [--tolerance 15] [--poll] [--log FILE]
"""
from __future__ import annotations

import argparse
import sys

from . import service
from .replay import load_track, parse_time, replay
from .sim import PATTERNS, Drag, simulate, write_gpx
from .watch import WatchConfig


def _rode(v: str) -> float:
    """The rode is a pick list in 50 ft steps (no chain counter aboard - 10-02)."""
    f = float(v)
    if f <= 0 or f % 50:
        raise argparse.ArgumentTypeError("rode is picked in 50 ft steps: 50, 100, 150 ...")
    return f


def _cmd_replay(a) -> int:
    fixes = load_track(a.track)
    arm_at = None
    if a.arm_at is not None and fixes:
        try:
            arm_at = fixes[0][0] + float(a.arm_at)
        except ValueError:
            arm_at = parse_time(a.arm_at)
    lines, _ = replay(fixes, WatchConfig(tolerance_m=a.tolerance, debounce_s=a.debounce), arm_at, a.every, a.rode_ft)
    print("\n".join(lines))
    return 0


def _cmd_sim(a) -> int:
    drag = Drag(a.drag, start_s=a.drag_at, speed_mps=a.drag_speed) if a.drag else None
    fixes = simulate(radius_m=a.radius, duration_s=a.hours * 3600, swing=a.pattern, drag=drag,
                     sigma_m=a.sigma, walk_sigma_m=a.walk, seed=a.seed)
    p = write_gpx(fixes, a.out, every_s=a.every)
    print(f"wrote {p} ({len(fixes)} fixes before thinning; anchor {fixes[0].anchor_lat:.6f},{fixes[0].anchor_lon:.6f})")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="anchorwatch", description="Circle-fit anchor watch (dry run).")
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("replay", help="replay a GPX / CSV track through the watch")
    r.add_argument("track")
    r.add_argument("--arm-at", help="seconds after the first fix, or an ISO time (default: first fix)")
    r.add_argument("--rode-ft", type=_rode, help="rode out, ft (50 ft steps): floors the no-lock circle, sizes the coarse guard")
    r.add_argument("--tolerance", type=float, default=15.0)
    r.add_argument("--debounce", type=float, default=20.0)
    r.add_argument("--every", type=float, default=600.0, help="status line every N s (0 = none)")
    r.set_defaults(fn=_cmd_replay)

    s = sub.add_parser("sim", help="write a synthetic swing (+ optional drag) as GPX")
    s.add_argument("--out", required=True)
    s.add_argument("--pattern", choices=sorted(PATTERNS), default="steady")
    s.add_argument("--hours", type=float, default=2.0)
    s.add_argument("--radius", type=float, default=40.0)
    s.add_argument("--drag", choices=["slow", "sudden"])
    s.add_argument("--drag-at", type=float, default=3600.0)
    s.add_argument("--drag-speed", type=float, default=0.05)
    s.add_argument("--sigma", type=float, default=2.5)
    s.add_argument("--walk", type=float, default=0.0, help="slow GPS wander sigma, m")
    s.add_argument("--every", type=float, default=1.0, help="GPX point spacing, s")
    s.add_argument("--seed", type=int, default=1)
    s.set_defaults(fn=_cmd_sim)

    u = sub.add_parser("run", help="DRY RUN against a SignalK server (loopback only by default)")
    u.add_argument("--signalk", default="http://127.0.0.1:3100")
    u.add_argument("--arm", action="store_true", help="arm at the first fix")
    u.add_argument("--follow-mode", action="store_true",
                   help="arm when the engine's boat mode (automation.mode) is ANCHOR, disarm otherwise (D-092)")
    u.add_argument("--rode-ft", type=_rode, help="rode out, ft (50 ft steps)")
    u.add_argument("--tolerance", type=float, default=15.0)
    u.add_argument("--debounce", type=float, default=20.0)
    u.add_argument("--poll", action="store_true", help="poll REST once a second instead of the WebSocket")
    u.add_argument("--status-every", type=float, default=60.0)
    u.add_argument("--log", help="also append the log to this file")
    u.add_argument("--allow-remote", action="store_true", help="permit a non-loopback SignalK (read only)")
    u.set_defaults(fn=service.main)

    a = ap.parse_args(argv)
    return a.fn(a)


if __name__ == "__main__":
    sys.exit(main())
