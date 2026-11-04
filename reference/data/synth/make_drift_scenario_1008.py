"""
Purpose: Generate the 10-08 synthetic anchor test track - an initial set on a ~150 ft swing in a 20 deg arc,
    then two straight 20 ft drags, each followed by a new 10 deg arc - as 1 Hz GPX + CSV (with heading) for the
    anchor-watch replay.
Version: v2 (2026-10-08) - v2: HEADING on every fix, pointed along the radius at the anchor (10-08: 'add a heading
    pointed at the circle radius'), + a constant compass offset and noise; CSV output (t, lat, lon, heading).
    v1 (2026-10-08)
Scope: STANDALONE UTILITY - test-data generator for the anchor-box reference model (ANQ-03 stand-in while the
    Montana's own walk data cannot be read off the unit).
Architecture: reference/data/synth. Pure stdlib, seeded RNG (reproducible). Writes GPX (position
    only) and CSV (position + heading) that `python -m anchorwatch replay` and tests/ read. Geometry in a local
    east/north frame (metres) about the first anchor position, equirectangular conversion (fine at < 1 km).
Invocation: cd reference; python data/synth/make_drift_scenario_1008.py [--seed 1] [--hdg-bias 8]
    [--hdg-noise 2] [--out FILE.gpx] -> data/synth/drift_scenario_1008.gpx + drift_scenario_1008.csv (default).
    The reference scenario, 10-08; the ASSUMED choices are listed below. Also imported by tests (build()).

The reference scenario (10-08): "150' radius with a 20 deg arc with the radius varying 10-30' over 15 mins (initial set)
then a straight drift back by 20' with a new 10 deg arc and then another 20' drift with yet another 10 deg arc".
ASSUMED - check your wiring:
  - 150 ft is the radius of the TRACKED POSITION (the GPS antenna) about the anchor, not the rode length;
  - "radius varying 10-30 ft" = the distance from the anchor runs 150 ft minus a slack of 10..30 ft (120-140 ft),
    wandering smoothly;
  - each 10 deg arc also lasts 15 min; each 20 ft drift takes 60 s (~0.1 m/s), straight downwind (away from the
    anchor along the arc's mean bearing); the anchor drags with her and re-sets 20 ft further downwind;
  - wind from the north (boat south of the anchor, mean bearing 180 deg); GPS noise 1.0 m sigma, 1 Hz;
  - heading = the bearing from the boat to the anchor (bow on the anchor) + a CONSTANT compass offset (default 8 deg -
    the CCU is dockside-calibrated only, real deviation unknown) + 2 deg sigma noise.
"""
from __future__ import annotations

import argparse
import math
import random
from datetime import datetime, timezone
from pathlib import Path

FT = 0.3048
ANCHOR_LAT, ANCHOR_LON = 29.9500, -90.0600      # same placeholder anchorage as anchorwatch.sim
EPOCH0 = 1_790_000_000.0                        # fixed start time so the file is reproducible
MEAN_BRG = 180.0                                # anchor -> boat, deg true (wind from the north)
R_FT = 150.0
SLACK_FT = (10.0, 30.0)
NOISE_M = 1.0


def _smooth(rng: random.Random, n: int, period_s: float) -> list[float]:
    """A smooth wander in [-1, 1]: sum of three sines with random phases (no jumps)."""
    ph = [rng.uniform(0, 2 * math.pi) for _ in range(3)]
    f = [1.0, 1.7, 2.9]
    out = [sum(math.sin(2 * math.pi * k * t / period_s + p) for k, p in zip(f, ph)) / 3.0 for t in range(n)]
    m = max(abs(v) for v in out) or 1.0
    return [v / m for v in out]


def _arc(rng, anchor_xy, arc_deg, secs):
    """Boat positions (x east, y north, metres) swinging in an arc of `arc_deg` about MEAN_BRG round anchor_xy."""
    swing = _smooth(rng, secs, 300.0)            # ~5 min swing period
    slack = _smooth(rng, secs, 420.0)
    lo, hi = SLACK_FT
    pts = []
    for i in range(secs):
        brg = math.radians(MEAN_BRG + swing[i] * arc_deg / 2.0)
        r = (R_FT - (lo + (hi - lo) * (slack[i] + 1) / 2.0)) * FT
        pts.append((anchor_xy[0] + r * math.sin(brg), anchor_xy[1] + r * math.cos(brg)))
    return pts


def _drift(start, dist_m, secs):
    """Straight line from `start`, `dist_m` further along MEAN_BRG (downwind), over `secs` seconds."""
    b = math.radians(MEAN_BRG)
    return [(start[0] + dist_m * (i + 1) / secs * math.sin(b), start[1] + dist_m * (i + 1) / secs * math.cos(b))
            for i in range(secs)]


def build(seed: int = 1, hdg_bias: float = 8.0, hdg_noise: float = 2.0):
    """Returns (fixes as (epoch, lat, lon, heading_deg), phase marks as (label, seconds from start), true anchor
    (x, y) per fix in metres about the first anchor)."""
    rng = random.Random(seed)
    anchor = (0.0, 0.0)
    xy, anc, marks = [], [], []
    marks.append(("set: 20 deg arc, 15 min", 0))
    xy += _arc(rng, anchor, 20.0, 900)
    anc += [anchor] * 900
    for n in (1, 2):
        marks.append((f"drift {n}: 20 ft straight back, 60 s", len(xy)))
        xy += _drift(xy[-1], 20 * FT, 60)
        anc += [(anchor[0], anchor[1] - 20 * FT * (i + 1) / 60) for i in range(60)]   # the anchor drags with her
        anchor = (anchor[0], anchor[1] - 20 * FT)            # anchor re-sets 20 ft further south
        arc = _arc(rng, anchor, 10.0, 900)
        dx, dy = xy[-1][0] - arc[0][0], xy[-1][1] - arc[0][1]   # blend so the new arc starts where the drift ended
        marks.append((f"arc {n + 1}: 10 deg, 15 min", len(xy)))
        xy += [(x + dx * max(0.0, 1 - i / 60), y + dy * max(0.0, 1 - i / 60)) for i, (x, y) in enumerate(arc)]
        anc += [anchor] * 900
    m_per_deg_lat = 111_320.0
    m_per_deg_lon = 111_320.0 * math.cos(math.radians(ANCHOR_LAT))
    fixes = []
    for i, (x, y) in enumerate(xy):
        ax, ay = anc[i]
        hdg = (math.degrees(math.atan2(ax - x, ay - y)) + hdg_bias + rng.gauss(0, hdg_noise)) % 360
        x += rng.gauss(0, NOISE_M)
        y += rng.gauss(0, NOISE_M)
        fixes.append((EPOCH0 + i, ANCHOR_LAT + y / m_per_deg_lat, ANCHOR_LON + x / m_per_deg_lon, hdg))
    return fixes, marks, anc


def write_gpx(fixes, path: Path) -> Path:
    def iso(t):
        return datetime.fromtimestamp(t, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    pts = "\n".join(f'      <trkpt lat="{la:.7f}" lon="{lo:.7f}"><time>{iso(t)}</time></trkpt>'
                    for t, la, lo, *_ in fixes)
    path.write_text('<?xml version="1.0" encoding="UTF-8"?>\n'
                    '<gpx version="1.1" creator="make_drift_scenario_1008.py" xmlns="http://www.topografix.com/GPX/1/1">\n'
                    '  <trk><name>10-08 synthetic: set 20 deg + 2 x (20 ft drift, 10 deg arc)</name><trkseg>\n'
                    f'{pts}\n  </trkseg></trk>\n</gpx>\n', encoding="utf-8")
    return path


def write_csv(fixes, path: Path) -> Path:
    with open(path, "w", encoding="utf-8", newline="") as fh:
        fh.write("t,lat,lon,heading\n")
        for t, la, lo, h in fixes:
            fh.write(f"{t:.0f},{la:.7f},{lo:.7f},{h:.1f}\n")
    return path


def main() -> None:
    ap = argparse.ArgumentParser(description="the 10-08 synthetic anchor drift track")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--hdg-bias", type=float, default=8.0)
    ap.add_argument("--hdg-noise", type=float, default=2.0)
    ap.add_argument("--out", type=Path, default=Path(__file__).with_name("drift_scenario_1008.gpx"))
    a = ap.parse_args()
    fixes, marks, _ = build(a.seed, a.hdg_bias, a.hdg_noise)
    write_gpx(fixes, a.out)
    write_csv(fixes, a.out.with_suffix(".csv"))
    print(f"wrote {a.out} + .csv - {len(fixes)} fixes, {len(fixes) / 60:.0f} min, heading offset {a.hdg_bias:+.0f} deg")
    for label, s in marks:
        print(f"  {s // 60:3.0f}:{s % 60:02.0f}  {label}")


if __name__ == "__main__":
    main()
