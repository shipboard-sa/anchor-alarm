"""
Purpose: Synthetic anchor-swing generator for testing the anchor watch - a boat on a circle of radius R
    round an anchor, the bearing driven by a wind-shift pattern, GPS Gaussian noise + occasional outlier
    jumps, 1 Hz, optional slow or sudden drag; seeded RNG, so every run is reproducible. Also writes GPX.
Version: v2 (2026-10-02) - v2: Stretch (a wind gust stretches the rode out, then it STOPS - 10-02). v1 (2026-09-30)
Scope: STANDALONE UTILITY - anchor-watch test/demo tooling.
Architecture: anchor-watch. Used by tests/ and by `python -m anchorwatch sim` (makes a GPX to feed
    `replay`). Pure stdlib (random.Random(seed)); geometry via geo.LocalFrame about the true anchor.
Invocation: imported (simulate(), PATTERNS, Drag, Stretch, write_gpx) or
    python -m anchorwatch sim --pattern steady --drag slow --drag-at 3600 --hours 2 --out swing.gpx
"""
from __future__ import annotations

import math
import random
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

from .geo import LocalFrame


@dataclass(frozen=True)
class Swing:
    """Bearing (anchor -> boat, deg true) over time: a base, an irregular oscillation, optional reversal."""
    base_deg: float = 0.0
    amplitude_deg: float = 35.0      # half-width of the main oscillation
    period_s: float = 480.0
    reversal_at_s: float | None = None
    reversal_s: float = 600.0        # the 180 deg reversal takes this long

    def bearing(self, t: float) -> float:
        a, p = self.amplitude_deg, self.period_s
        b = self.base_deg + a * math.sin(2 * math.pi * t / p) + 0.25 * a * math.sin(2 * math.pi * t / (0.37 * p) + 1.3)
        if self.reversal_at_s is not None:
            b += 180.0 * min(1.0, max(0.0, (t - self.reversal_at_s) / self.reversal_s))
        return b


PATTERNS = {
    "steady": Swing(amplitude_deg=35.0),              # ~85 deg peak to peak
    "large": Swing(amplitude_deg=60.0),               # ~150 deg
    "small": Swing(amplitude_deg=4.0),                # ~10 deg
    "none": Swing(amplitude_deg=0.5),                 # hanging dead to the tide
    "reversal": Swing(amplitude_deg=35.0, reversal_at_s=1800.0),  # swing, then wind/tide turns 180 at 30 min
    "small-reversal": Swing(amplitude_deg=4.0, reversal_at_s=1800.0),  # little swing (no lock), then 180
}


@dataclass(frozen=True)
class Drag:
    mode: str = "slow"               # "slow" | "sudden"
    start_s: float = 3600.0
    speed_mps: float = 0.05          # slow drag speed (also the slide after a sudden jump)
    jump_m: float = 30.0             # sudden: the anchor lets go and moves this far ...
    jump_s: float = 30.0             # ... in this long, then keeps sliding at speed_mps
    direction_deg: float | None = None   # default: downwind = the boat's bearing at start_s

    def moved(self, t: float) -> float:
        dt = t - self.start_s
        if dt <= 0:
            return 0.0
        if self.mode == "slow":
            return self.speed_mps * dt
        if dt < self.jump_s:
            return self.jump_m * dt / self.jump_s
        return self.jump_m + self.speed_mps * (dt - self.jump_s)


@dataclass(frozen=True)
class Stretch:
    """A wind gust / fresh breeze stretching the rode: the radius grows by extra_m over ramp_s from start_s,
    holds for hold_s, then eases back over ramp_s. It STOPS - unlike a drag (10-02)."""
    start_s: float = 3600.0
    extra_m: float = 8.0
    ramp_s: float = 60.0
    hold_s: float = 1800.0

    def extra(self, t: float) -> float:
        dt = t - self.start_s
        if dt <= 0:
            return 0.0
        if dt < self.ramp_s:
            return self.extra_m * dt / self.ramp_s
        dt -= self.ramp_s
        if dt < self.hold_s:
            return self.extra_m
        dt -= self.hold_s
        return self.extra_m * max(0.0, 1.0 - dt / self.ramp_s)


@dataclass(frozen=True)
class SimFix:
    t: float
    lat: float
    lon: float
    anchor_lat: float     # truth
    anchor_lon: float
    boat_lat: float       # truth, before GPS noise
    boat_lon: float
    outlier: bool


def simulate(anchor_lat: float = 29.9500, anchor_lon: float = -90.0600, radius_m: float = 40.0,
             duration_s: float = 3600.0, swing: Swing | str = "steady", drag: Drag | None = None,
             stretch: Stretch | None = None,
             sigma_m: float = 2.5, walk_sigma_m: float = 0.0, walk_tau_s: float = 120.0, outlier_prob: float = 0.003, outlier_m: tuple[float, float] = (20.0, 60.0),
             rate_hz: float = 1.0, seed: int = 1, t0: float = 0.0) -> list[SimFix]:
    """Generate fixes at rate_hz from t0 to t0 + duration_s. Times are t0 + seconds.
    Noise = white Gaussian sigma_m per axis + an optional slow wander (first-order Gauss-Markov,
    steady-state sigma walk_sigma_m, time constant walk_tau_s) - real GPS error is correlated in time."""
    sw = PATTERNS[swing] if isinstance(swing, str) else swing
    rng = random.Random(seed)
    frame = LocalFrame(anchor_lat, anchor_lon)
    drag_dir = None
    if drag is not None:
        drag_dir = math.radians(drag.direction_deg if drag.direction_deg is not None else sw.bearing(drag.start_s))
    out: list[SimFix] = []
    wx = wy = 0.0
    phi = math.exp(-1.0 / (rate_hz * walk_tau_s)) if walk_sigma_m > 0 else 0.0
    q = walk_sigma_m * math.sqrt(1.0 - phi * phi)
    n = int(duration_s * rate_hz)
    for i in range(n + 1):
        s = i / rate_hz
        ax = ay = 0.0
        if drag is not None:
            m = drag.moved(s)
            ax, ay = m * math.sin(drag_dir), m * math.cos(drag_dir)
        b = math.radians(sw.bearing(s))
        r = radius_m + (stretch.extra(s) if stretch is not None else 0.0)
        bx, by = ax + r * math.sin(b), ay + r * math.cos(b)
        if walk_sigma_m > 0:
            wx, wy = phi * wx + rng.gauss(0.0, q), phi * wy + rng.gauss(0.0, q)
        gx, gy = bx + wx + rng.gauss(0.0, sigma_m), by + wy + rng.gauss(0.0, sigma_m)
        outlier = rng.random() < outlier_prob
        if outlier:
            d, th = rng.uniform(*outlier_m), rng.uniform(0.0, 2 * math.pi)
            gx, gy = gx + d * math.sin(th), gy + d * math.cos(th)
        lat, lon = frame.to_latlon(gx, gy)
        alat, alon = frame.to_latlon(ax, ay)
        blat, blon = frame.to_latlon(bx, by)
        out.append(SimFix(t0 + s, lat, lon, alat, alon, blat, blon, outlier))
    return out


def write_gpx(fixes, path: str | Path, every_s: float = 1.0, epoch0: float = 1_790_000_000.0) -> Path:
    """Write fixes as a GPX 1.1 track. Times = epoch0 + fix.t, whole seconds; every_s thins the track
    (a handheld often logs every 1-5 s)."""
    lines = ['<?xml version="1.0" encoding="UTF-8"?>',
             '<gpx version="1.1" creator="anchorwatch.sim" xmlns="http://www.topografix.com/GPX/1/1">',
             "<trk><name>anchorwatch synthetic swing</name><trkseg>"]
    last = None
    for f in fixes:
        if last is not None and f.t - last < every_s - 1e-9:
            continue
        last = f.t
        ts = datetime.fromtimestamp(round(epoch0 + f.t), tz=timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
        lines.append(f'<trkpt lat="{f.lat:.7f}" lon="{f.lon:.7f}"><time>{ts}</time></trkpt>')
    lines.append("</trkseg></trk></gpx>")
    p = Path(path)
    p.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return p
