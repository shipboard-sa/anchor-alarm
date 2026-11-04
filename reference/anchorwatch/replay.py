"""
Purpose: Replay a recorded track (GPX trkpt or CSV t,lat,lon) through the anchor watch and print a
    timeline: ARM, lock time + fitted centre/radius, conservative fallback, alarms, periodic status.
Version: v3 (2026-10-08) - v3: optional heading (CSV column heading / hdg, deg true) passed to the watch - the v4 heading lock. v2 (2026-10-02) - v2: rode_ft (--rode-ft) passed to ARM; DRAG? lines. v1 (2026-09-30)
Scope: STANDALONE UTILITY - anchor-watch tuning/testing tool (reads a file, writes only stdout).
Architecture: anchor-watch. Parses the track (stdlib xml.etree / csv), feeds watch.AnchorWatch in time
    order, formats its events. Built for a handheld-GPS test walk (circles, arcs, simulated drags)
    and the shakedown anchor recordings; any GPX with times works, 1-5 s resolution included.
Invocation: python -m anchorwatch replay TRACK.gpx|TRACK.csv [--arm-at SECONDS|ISO] [--rode-ft 150] [--tolerance 15]
    [--debounce 20] [--every 600]   (from reference). Also imported by tests (load_track, replay).
"""
from __future__ import annotations

import csv
import xml.etree.ElementTree as ET
from datetime import datetime, timezone
from pathlib import Path

from .watch import AnchorWatch, WatchConfig

Fix = tuple[float, float, float]   # (epoch seconds, lat, lon)


def parse_time(s: str) -> float:
    """ISO-8601 (GPX style, 'Z' or offset, optional fraction) or plain epoch seconds -> epoch seconds."""
    s = s.strip()
    try:
        return float(s)
    except ValueError:
        pass
    d = datetime.fromisoformat(s.replace("Z", "+00:00"))
    if d.tzinfo is None:
        d = d.replace(tzinfo=timezone.utc)
    return d.timestamp()


def load_gpx(path: str | Path) -> list[Fix]:
    """All trkpt (falling back to rtept) with a <time>, any GPX version / namespace."""
    root = ET.parse(path).getroot()
    out: list[Fix] = []
    for tag in ("trkpt", "rtept"):
        for el in root.iter():
            if el.tag.rsplit("}", 1)[-1] != tag:
                continue
            tm = next((c.text for c in el if c.tag.rsplit("}", 1)[-1] == "time" and c.text), None)
            if tm is None:
                continue
            out.append((parse_time(tm), float(el.get("lat")), float(el.get("lon"))))
        if out:
            break
    return out


def load_csv(path: str | Path) -> list[Fix]:
    """CSV with a header naming t (or time), lat, lon [, heading]; t = epoch seconds or ISO-8601."""
    out: list = []
    with open(path, newline="", encoding="utf-8-sig") as fh:
        for row in csv.DictReader(fh):
            row = {k.strip().lower(): v for k, v in row.items() if k}
            t = row.get("t") or row.get("time") or row.get("timestamp")
            if t and row.get("lat") and row.get("lon"):
                h = row.get("heading") or row.get("hdg")
                fix = (parse_time(t), float(row["lat"]), float(row["lon"]))
                out.append(fix + (float(h),) if h not in (None, "") else fix)
    return out


def load_track(path: str | Path) -> list[Fix]:
    p = Path(path)
    fixes = load_gpx(p) if p.suffix.lower() == ".gpx" else load_csv(p)
    fixes.sort(key=lambda f: f[0])
    return fixes


def _hms(s: float) -> str:
    s = int(round(s))
    return f"{s // 3600:02d}:{s % 3600 // 60:02d}:{s % 60:02d}"


def replay(fixes: list[Fix], cfg: WatchConfig | None = None, arm_at: float | None = None,
           every_s: float = 600.0, rode_ft: float | None = None) -> tuple[list[str], AnchorWatch]:
    """Run the watch over the fixes. arm_at = absolute epoch (default: the first fix).
    Returns the timeline lines and the watch (for its final status)."""
    lines: list[str] = []
    w = AnchorWatch(cfg)
    if not fixes:
        return ["no fixes in track"], w
    t_first = fixes[0][0]
    arm_t = t_first if arm_at is None else arm_at
    iso = lambda t: datetime.fromtimestamp(t, tz=timezone.utc).strftime("%Y-%m-%d %H:%M:%SZ")
    lines.append(f"track: {len(fixes)} fixes, {iso(t_first)} .. {iso(fixes[-1][0])} "
                 f"({_hms(fixes[-1][0] - t_first)})")

    def emit(evs):
        for e in evs:
            tag = e.kind if e.kind != "STATE" else f"{e.prev} -> {e.state}"
            lines.append(f"{_hms(e.t - t_first)}  {tag:<26} {e.message}")

    armed = False
    next_report = arm_t + every_s
    for t, lat, lon, *rest in fixes:
        if not armed and t >= arm_t:
            emit(w.arm(t, rode_ft))
            armed = True
            next_report = t + every_s
        if not armed:
            continue
        emit(w.tick(t))
        emit(w.update(t, lat, lon, rest[0] if rest else None))
        if every_s > 0 and t >= next_report:
            s = w.status(t)
            d = f"{s.distance_m:.1f}" if s.distance_m is not None else "-"
            ar = f"{s.alarm_radius_m:.1f}" if s.alarm_radius_m is not None else "-"
            extra = f"  ({s.no_lock_reason})" if s.no_lock_reason else ""
            lines.append(f"{_hms(t - t_first)}  status {s.state:<18} dist {d} m / alarm {ar} m{extra}")
            next_report += every_s
    s = w.status()
    lines.append(f"end: state {s.state}; accepted {s.fixes_accepted} fixes; rejects {s.rejects}")
    if s.centre_lat is not None:
        lines.append(f"     centre {s.centre_lat:.6f},{s.centre_lon:.6f} radius {s.radius_m:.1f} m"
                     + (f" alarm circle {s.alarm_radius_m:.1f} m" if s.alarm_radius_m else ""))
    return lines, w
