"""
Purpose: The anchor-watch state machine - ARM, observe the swing, fit a circle (centre = anchor), LOCK,
    freeze it, and ALARM when the boat stays outside fitted radius + tolerance. Clock-free and
    deterministic: the caller passes every timestamp.
Version: v4 (2026-10-08) - v4: HEADING LOCK for a narrow swing (10-08: "if we are sitting in a strong current or wind she
    may not sail that much side to side"; "if heading will help use it" - the CCU heading, dockside-calibrated only).
    The bow points at the anchor, so each 10-s bin gives a ray (position + heading); the rays cross at the anchor.
    Used only when the circle fit cannot lock; the alarm circle is still OBSERVED distance + tolerance, plus a
    margin for a constant compass error (2 x range x sin(hdg_bias_deg)) so a tide reversal never false-alarms
    (SPEC 5.10). The rode only sanity-bounds the range, it never sets the circle (SPEC 5.5). v3 (2026-10-02) - v3: snapshot() / restore() - the watch survives a power cut, no age limit (10-02). v2 (10-02, ./SPEC.md s.5): rode pick at ARM (50 ft steps, a guess -
    no chain counter); bow-to-antenna 60 ft; the CONSERVATIVE circle is floored at 2 x (rode + bow-to-antenna);
    COARSE GUARD while ACQUIRING; DRAG? warning ('a wind stretch stops, a drag keeps going'); water speed + wind
    as CONTEXT only (shown in messages, never change the circle). A LOCKED circle stays observed geometry +
    tolerance - the rode never sets it. v1 (2026-09-30).
Scope: STANDALONE UTILITY - anchor-watch module (dry-run only: it computes state, it drives nothing).
Architecture: anchor-watch core. Callers: replay.py (recorded GPX/CSV), service.py (live SignalK,
    dry run), tests. Uses geo.LocalFrame (metres about the first fix after ARM) and fit.fit_circle().
    States: OFF -> ACQUIRING -> LOCKED -> ALARM, plus CONSERVATIVE (no lock by no_lock_after_s: a frozen
    centroid circle while it keeps trying to lock). disarm() -> OFF from anywhere; ALARM is latched
    until disarm() or a fresh arm(). The boat is safe ANYWHERE inside the circle - only the outer edge alarms.
    This is the reference for the anchor box firmware (., a C port with the same tests).
Invocation: imported. w = AnchorWatch(WatchConfig(...)); w.arm(t, rode_ft=150); events = w.update(t, lat, lon[, heading_deg]);
    w.context(t, stw_mps=..., wind_mps=...) for conditions; w.tick(t) for GPS-stale warnings; w.status().
"""
from __future__ import annotations

import math
from collections import deque
from dataclasses import dataclass, field

from .fit import CircleFit, fit_circle
from .geo import LocalFrame

OFF, ACQUIRING, LOCKED, CONSERVATIVE, ALARM = "OFF", "ACQUIRING", "LOCKED", "CONSERVATIVE", "ALARM"
FT = 0.3048
KN = 1852.0 / 3600.0              # m/s per knot


@dataclass
class WatchConfig:
    tolerance_m: float = 15.0        # alarm circle = fitted radius + this
    debounce_s: float = 20.0         # continuously outside this long -> ALARM
    # fix filter
    max_speed_mps: float = 3.0       # a fix further than max_speed * dt + jump_margin from the last
    jump_margin_m: float = 10.0      #   accepted fix is a GPS jump and is rejected (margin = GPS noise)
    reject_reset_count: int = 10     # this many jump-rejects in a row -> accept (never hide a real move)
    # fitting
    bin_s: float = 10.0              # fixes averaged into bins of this length before fitting
    fit_window_s: float = 900.0      # sliding window of bins used for each (re)fit
    min_fit_bins: int = 12
    # lock rule - ALL must hold
    lock_min_s: float = 600.0        # observed at least 10 min
    min_arc_deg: float = 40.0
    max_rms_m: float = 4.0
    centre_stable_m: float = 3.0     # fitted centre moved less than this ...
    centre_stable_s: float = 180.0   # ... over the last 3 min
    min_radius_m: float = 15.0       # rode + bow-to-antenna is never this short on a 51' boat; below
                                     #   it a GPS-wander blob can fake a tiny circle (seen in sim)
    max_radius_m: float = 250.0
    min_sagitta_ratio: float = 3.0   # arc depth must be >= this x rms (curvature really resolved)
    # fallback
    no_lock_after_s: float = 900.0   # no lock by 15 min -> CONSERVATIVE
    stale_after_s: float = 30.0      # tick(): warn once when no accepted fix for this long
    # boat geometry (10-02)
    bow_to_antenna_m: float = 60 * FT  # the MSC-10 (position source) is ~60 ft aft of the bow roller
    # DRAG? warning: distance from the frozen centre still rising after trend_window_s, not levelling off
    trend_window_s: float = 720.0    # judged over the last 12 min ...
    trend_segments: int = 3          # ... split in thirds: EVERY third must be rising. A wind stretch rises in one
                                     #   burst then stops (one third rises, the rest are flat); a drag keeps going
    trend_min_slope_mps: float = 0.015   # whole-window rate, ~0.9 m/min (~0.03 kn); each third >= half of it
    trend_min_rise_m: float = 12.0   # rise over the window. 720 s / 12 m (sim 10-02): no warn in 48 h at 2 m GPS
                                     #   wander (2 at 3 m); warns a 0.02 m/s drag 5-7 min, 0.03 m/s ~2 min before
                                     #   the ALARM. Faster drags reach the circle first. Tune on real swings
    context_fresh_s: float = 60.0    # water speed / wind older than this is not quoted
    # HEADING LOCK (v4, 10-08) - a narrow swing the circle fit cannot resolve; needs heading with the fixes
    heading_lock: bool = True
    hdg_min_bins: int = 30           # 5 min of 10-s bins carrying a heading
    hdg_min_spread_deg: float = 6.0  # the bow must have swung at least this much (rays must cross, not run parallel)
    hdg_max_rms_m: float = 8.0       # rays' RMS miss distance from the solved anchor
    hdg_bias_deg: float = 10.0       # ASSUMED worst constant compass error (CCU, dockside calibration only) + the bow
                                     #   not pointing exactly at the anchor; widens the circle by 2 x range x sin(this)
    hdg_centre_stable_m: float = 6.0 # solved anchor moved less than this over centre_stable_s


@dataclass(frozen=True)
class Event:
    t: float
    kind: str          # "STATE" (state change) | "WARN" | "DRAG" (DRAG? warning) | "INFO"
    state: str         # state after the event
    prev: str | None
    message: str


@dataclass(frozen=True)
class Status:
    state: str
    armed_at: float | None
    elapsed_s: float | None
    centre_lat: float | None       # locked / conservative centre, or the provisional fit while ACQUIRING
    centre_lon: float | None
    radius_m: float | None
    alarm_radius_m: float | None
    distance_m: float | None       # last accepted fix to the centre
    arc_span_deg: float | None
    rms_m: float | None
    no_lock_reason: str | None
    locked_at: float | None
    alarm_at: float | None
    fixes_accepted: int
    rejects: dict
    gps_stale: bool
    rode_ft: float | None = None
    reach_m: float | None = None     # rode + bow-to-antenna: the most the antenna can be from the anchor
    drag_suspected: bool = False
    stw_kn: float | None = None      # context only (fresh values)
    wind_kn: float | None = None


class AnchorWatch:
    def __init__(self, cfg: WatchConfig | None = None):
        self.cfg = cfg or WatchConfig()
        self.state = OFF
        # conditions belong to the boat, not to one watch: kept across arm / disarm
        self.stw: tuple[float, float] | None = None              # (t, m/s)
        self.wind: tuple[float, float] | None = None
        self._reset()

    # ---- control -------------------------------------------------------------------------------
    def _reset(self) -> None:
        self.armed_at: float | None = None
        self.frame: LocalFrame | None = None
        self.last_t: float | None = None
        self.last_xy: tuple[float, float] | None = None
        self.last_latlon: tuple[float, float] | None = None
        self.jump_streak = 0
        self.accepted = 0
        self.rejects = {"duplicate": 0, "jump": 0, "repeat": 0, "filter_reset": 0}
        self.raw: list[tuple[float, float]] = []                 # accepted fixes, ACQUIRING only
        self.bins: deque = deque()                               # (t_mid, x, y)
        self._bin: list | None = None                            # [index, sum_t, sum_x, sum_y, n, sum_sin, sum_cos, n_hdg]
        self.hbins: deque = deque()                              # (t_mid, x, y, heading_rad) - bins with a heading
        self.hdg_hist: deque = deque()                           # (t, ax, ay) of sane heading solutions
        self.lock_kind: str | None = None                        # "circle" | "heading"
        self.fit: CircleFit | None = None
        self.centre_hist: deque = deque()                        # (t, cx, cy) of sane fits
        self.no_lock_reason: str | None = None
        self.centre: tuple[float, float] | None = None           # frozen (x, y)
        self.radius: float | None = None
        self.alarm_radius: float | None = None
        self.locked_at: float | None = None
        self.alarm_at: float | None = None
        self.outside_since: float | None = None
        self.distance: float | None = None
        self.stale = False
        self.rode_ft: float | None = None
        self.reach: float | None = None
        self.arm_point: tuple[float, float] | None = None       # first accepted fix after ARM (x, y)
        self.trend: deque = deque()                              # (t_mid, mean distance from centre)
        self._tbin: list | None = None                           # [index, sum_t, sum_d, n]
        self.drag_suspected = False

    def _change(self, t: float, new: str, msg: str) -> Event:
        ev = Event(t, "STATE", new, self.state, msg)
        self.state = new
        return ev

    def arm(self, t: float, rode_ft: float | None = None) -> list[Event]:
        """Start (or restart) a watch. Clears any latched ALARM and all previous fits.
        rode_ft = the crew's pick of rode out (50 ft steps; a guess). It floors only the CONSERVATIVE
        circle and sizes the coarse guard; a LOCKED circle comes from the observed swing alone."""
        prev = self.state
        self._reset()
        self.armed_at = t
        if rode_ft is not None and rode_ft > 0:
            self.rode_ft = float(rode_ft)
            self.reach = self.rode_ft * FT + self.cfg.bow_to_antenna_m
        self.state = prev
        msg = "armed - observing the swing"
        msg += (f"; rode {self.rode_ft:.0f} ft -> reach {self.reach:.0f} m, coarse guard "
                f"{self._guard_radius():.0f} m from here" if self.reach else "; no rode given - no coarse guard")
        return [self._change(t, ACQUIRING, msg)]

    def context(self, t: float, stw_mps: float | None = None, wind_mps: float | None = None) -> None:
        """Conditions (water speed, wind). CONTEXT ONLY: quoted in warnings / alarms, never change the circle."""
        if stw_mps is not None:
            self.stw = (t, float(stw_mps))
        if wind_mps is not None:
            self.wind = (t, float(wind_mps))

    def _fresh(self, v, t):
        return v[1] if v is not None and t - v[0] <= self.cfg.context_fresh_s else None

    def _ctx(self, t: float) -> str:
        parts = []
        w, s = self._fresh(self.wind, t), self._fresh(self.stw, t)
        if w is not None:
            parts.append(f"wind {w / KN:.0f} kn")
        if s is not None:
            parts.append(f"water speed {s / KN:.1f} kn" + (" (moving with the water?)" if s / KN < 0.1 else ""))
        return (" | " + ", ".join(parts)) if parts else ""

    def _guard_radius(self) -> float:
        # armed ~one reach from the anchor; the far side of the circle is 2 x reach away
        return 2 * self.reach + self.cfg.tolerance_m

    def disarm(self, t: float) -> list[Event]:
        if self.state == OFF:
            return []
        ev = self._change(t, OFF, "disarmed")
        self._reset()
        return [ev]

    # ---- input ---------------------------------------------------------------------------------
    def tick(self, t: float) -> list[Event]:
        """Call periodically (e.g. 1 Hz) so a GPS that goes quiet is reported."""
        if self.state == OFF or self.stale:
            return []
        ref = self.last_t if self.last_t is not None else self.armed_at
        if ref is not None and t - ref > self.cfg.stale_after_s:
            self.stale = True
            return [Event(t, "WARN", self.state, None,
                          f"GPS stale: no accepted fix for {t - ref:.0f} s - watch is blind")]
        return []

    def update(self, t: float, lat: float, lon: float, heading_deg: float | None = None) -> list[Event]:
        """Feed one position fix (+ optional true heading, deg). Returns the events it caused."""
        if self.state == OFF:
            return []
        c = self.cfg
        events: list[Event] = []
        if self.frame is None:
            self.frame = LocalFrame(lat, lon)
        x, y = self.frame.to_xy(lat, lon)

        if self.last_t is not None:
            if t <= self.last_t:
                self.rejects["duplicate"] += 1
                return events
            dt = t - self.last_t
            if math.hypot(x - self.last_xy[0], y - self.last_xy[1]) > c.max_speed_mps * dt + c.jump_margin_m:
                self.jump_streak += 1
                if self.jump_streak < c.reject_reset_count:
                    self.rejects["jump"] += 1
                    return events
                self.rejects["filter_reset"] += 1
                events.append(Event(t, "WARN", self.state, None,
                                    f"{self.jump_streak} jumps in a row - accepting the new position"))
        self.jump_streak = 0
        if self.arm_point is None:
            self.arm_point = (x, y)
        repeat = (lat, lon) == self.last_latlon
        self.last_t, self.last_xy, self.last_latlon = t, (x, y), (lat, lon)
        self.accepted += 1
        if self.stale:
            self.stale = False
            events.append(Event(t, "INFO", self.state, None, "GPS fixes resumed"))

        if repeat:
            self.rejects["repeat"] += 1        # a stale repeat says nothing new about the swing
        elif self.state in (ACQUIRING, CONSERVATIVE):
            if self.state == ACQUIRING:
                self.raw.append((x, y))
            self._add_to_bin(t, x, y, events, heading_deg)

        elapsed = t - self.armed_at
        if self.state == ACQUIRING and elapsed >= c.no_lock_after_s:
            self._go_conservative(t, events)
        if self.state in (LOCKED, CONSERVATIVE, ALARM):
            self._check_circle(t, x, y, events)
            self._add_trend(t, events)
        else:
            if self.fit is not None:
                self.distance = math.hypot(x - self.fit.cx, y - self.fit.cy)
            if self.reach is not None:
                self._check_guard(t, x, y, events)
        return events

    # ---- fitting / lock ------------------------------------------------------------------------
    def _add_to_bin(self, t: float, x: float, y: float, events: list[Event], heading_deg: float | None = None) -> None:
        idx = int((t - self.armed_at) // self.cfg.bin_s)
        if self._bin is not None and self._bin[0] != idx:
            _, st, sx, sy, n, ss, sc, nh = self._bin
            self.bins.append((st / n, sx / n, sy / n))
            if nh and nh * 2 >= n:                               # a heading for most of the bin
                self.hbins.append((st / n, sx / n, sy / n, math.atan2(ss, sc)))
            self._bin = None
            self._refit(t, events)
        if self._bin is None:
            self._bin = [idx, 0.0, 0.0, 0.0, 0, 0.0, 0.0, 0]
        b = self._bin
        b[1] += t; b[2] += x; b[3] += y; b[4] += 1
        if heading_deg is not None and math.isfinite(heading_deg):
            h = math.radians(heading_deg)
            b[5] += math.sin(h); b[6] += math.cos(h); b[7] += 1

    def _refit(self, t: float, events: list[Event]) -> None:
        c = self.cfg
        while self.bins and self.bins[0][0] < t - c.fit_window_s:
            self.bins.popleft()
        while self.hbins and self.hbins[0][0] < t - c.fit_window_s:
            self.hbins.popleft()
        if len(self.bins) < c.min_fit_bins:
            self.no_lock_reason = f"collecting ({len(self.bins)}/{c.min_fit_bins} averaged positions)"
            return
        f = fit_circle([(bx, by) for _, bx, by in self.bins])
        self.fit = f
        if f is None or not (c.min_radius_m <= f.r <= c.max_radius_m):
            self.centre_hist.clear()
            self.no_lock_reason = ("fit degenerate" if f is None else
                                   f"radius {f.r:.0f} m outside {c.min_radius_m:.0f}..{c.max_radius_m:.0f} m")
            self._try_heading_lock(t, events)
            return
        self.centre_hist.append((t, f.cx, f.cy))
        while self.centre_hist and self.centre_hist[0][0] < t - c.centre_stable_s - 2 * c.bin_s:
            self.centre_hist.popleft()

        why = []
        elapsed = t - self.armed_at
        if elapsed < c.lock_min_s:
            why.append(f"observed {elapsed / 60:.1f}/{c.lock_min_s / 60:.0f} min")
        if f.arc_span_deg < c.min_arc_deg:
            why.append(f"arc {f.arc_span_deg:.0f}/{c.min_arc_deg:.0f} deg")
        if f.rms > c.max_rms_m:
            why.append(f"rms {f.rms:.1f} > {c.max_rms_m:.1f} m")
        if f.sagitta < c.min_sagitta_ratio * f.rms:
            why.append(f"curvature not resolved (arc depth {f.sagitta:.1f} m vs rms {f.rms:.1f} m)")
        oldest = self.centre_hist[0][0]
        if t - oldest < c.centre_stable_s:
            why.append(f"centre history {t - oldest:.0f}/{c.centre_stable_s:.0f} s")
        else:
            moved = max(math.hypot(f.cx - hx, f.cy - hy) for ht, hx, hy in self.centre_hist
                        if ht >= t - c.centre_stable_s)
            if moved >= c.centre_stable_m:
                why.append(f"centre moved {moved:.1f} m in {c.centre_stable_s / 60:.0f} min")
        if why:
            self.no_lock_reason = "; ".join(why)
            self._try_heading_lock(t, events)
            return
        self._lock(t, f, events)

    # ---- heading lock (v4) -----------------------------------------------------------------------
    def _solve_rays(self):
        """Least-squares crossing of the bow rays (position + heading) -> (ax, ay, rms_m, spread_deg, ahead_frac)
        or None. Each ray: p + s * u, u = (sin h, cos h); minimises the squared perpendicular misses."""
        hb = list(self.hbins)
        a11 = a12 = a22 = b1 = b2 = 0.0
        for _, px, py, h in hb:
            ux, uy = math.sin(h), math.cos(h)
            m11, m12, m22 = 1 - ux * ux, -ux * uy, 1 - uy * uy          # I - u u^T
            a11 += m11; a12 += m12; a22 += m22
            b1 += m11 * px + m12 * py; b2 += m12 * px + m22 * py
        det = a11 * a22 - a12 * a12
        if det <= 1e-9 * len(hb) ** 2:
            return None
        ax, ay = (a22 * b1 - a12 * b2) / det, (a11 * b2 - a12 * b1) / det
        miss2, ahead = 0.0, 0
        for _, px, py, h in hb:
            ux, uy = math.sin(h), math.cos(h)
            dx, dy = ax - px, ay - py
            along = dx * ux + dy * uy
            ahead += along > 0
            miss2 += (dx - along * ux) ** 2 + (dy - along * uy) ** 2
        mean = math.atan2(sum(math.sin(h) for *_, h in hb), sum(math.cos(h) for *_, h in hb))
        dev = [math.degrees((h - mean + math.pi) % (2 * math.pi) - math.pi) for *_, h in hb]
        return ax, ay, math.sqrt(miss2 / len(hb)), max(dev) - min(dev), ahead / len(hb)

    def _try_heading_lock(self, t: float, events: list[Event]) -> None:
        """Narrow swing: lock on where the bow rays cross. Same 'observed + stable' discipline as the circle lock."""
        c = self.cfg
        if not c.heading_lock or len(self.hbins) < c.hdg_min_bins:
            return
        sol = self._solve_rays()
        if sol is None:
            self.hdg_hist.clear()
            return
        ax, ay, rms, spread, ahead = sol
        rng = sum(math.hypot(px - ax, py - ay) for _, px, py, _h in self.hbins) / len(self.hbins)
        upper = (self.reach + c.tolerance_m) if self.reach else c.max_radius_m
        why = []
        if t - self.armed_at < c.lock_min_s:
            why.append("observing")
        if spread < c.hdg_min_spread_deg:
            why.append(f"heading spread {spread:.0f}/{c.hdg_min_spread_deg:.0f} deg")
        if rms > c.hdg_max_rms_m:
            why.append(f"rays miss by {rms:.1f} m rms")
        if ahead < 0.9:
            why.append("anchor not ahead of the bow")
        if not (c.min_radius_m <= rng <= upper):
            why.append(f"range {rng:.0f} m outside {c.min_radius_m:.0f}..{upper:.0f} m")
        if why:
            self.hdg_hist.clear()
            self.no_lock_reason = (self.no_lock_reason or "") + "; heading: " + ", ".join(why)
            return
        self.hdg_hist.append((t, ax, ay))
        while self.hdg_hist and self.hdg_hist[0][0] < t - c.centre_stable_s - 2 * c.bin_s:
            self.hdg_hist.popleft()
        if t - self.hdg_hist[0][0] < c.centre_stable_s:
            self.no_lock_reason = (self.no_lock_reason or "") + \
                f"; heading: anchor history {t - self.hdg_hist[0][0]:.0f}/{c.centre_stable_s:.0f} s"
            return
        moved = max(math.hypot(ax - hx, ay - hy) for ht, hx, hy in self.hdg_hist if ht >= t - c.centre_stable_s)
        if moved >= c.hdg_centre_stable_m:
            self.no_lock_reason = (self.no_lock_reason or "") + f"; heading: anchor moved {moved:.1f} m"
            return
        seen = max(math.hypot(bx - ax, by - ay) for _, bx, by in self.bins)
        margin = 2 * rng * math.sin(math.radians(c.hdg_bias_deg))
        self.centre, self.radius = (ax, ay), rng
        self.alarm_radius = seen + margin + c.tolerance_m
        self.locked_at, self.no_lock_reason, self.lock_kind = t, None, "heading"
        self.outside_since = None
        self._reset_trend()
        self.raw.clear(); self.bins.clear(); self.hbins.clear(); self._bin = None
        lat, lon = self.frame.to_latlon(ax, ay)
        events.append(self._change(t, LOCKED,
            f"LOCK (heading): anchor {lat:.6f},{lon:.6f} range {rng:.1f} m (max seen {seen:.1f} m) "
            f"alarm circle {self.alarm_radius:.1f} m (+{margin:.1f} m compass margin; bow swing {spread:.0f} deg, "
            f"rays miss {rms:.1f} m rms)"))

    def _lock(self, t: float, f: CircleFit, events: list[Event]) -> None:
        self.centre, self.radius = (f.cx, f.cy), f.r          # FROZEN from here on
        # never smaller than where the boat was already seen swinging (10-s averages), so a fit that
        # came out short on noisy data cannot false-alarm on the swing it was fitted to
        seen = max(math.hypot(bx - f.cx, by - f.cy) for _, bx, by in self.bins)
        self.alarm_radius = max(f.r, seen) + self.cfg.tolerance_m
        self.locked_at, self.no_lock_reason, self.lock_kind = t, None, "circle"
        self.outside_since = None
        self._reset_trend()
        self.raw.clear(); self.bins.clear(); self.hbins.clear(); self._bin = None
        lat, lon = self.frame.to_latlon(*self.centre)
        events.append(self._change(t, LOCKED,
            f"LOCK: anchor {lat:.6f},{lon:.6f} radius {f.r:.1f} m (max seen {seen:.1f} m) "
            f"alarm circle {self.alarm_radius:.1f} m "
            f"(arc {f.arc_span_deg:.0f} deg, rms {f.rms:.1f} m, {f.n} averaged positions)"))

    def _go_conservative(self, t: float, events: list[Event]) -> None:
        pts = list(self.raw) or [self.last_xy]
        cx = sum(p[0] for p in pts) / len(pts)
        cy = sum(p[1] for p in pts) / len(pts)
        self.centre = (cx, cy)
        self.radius = max(math.hypot(px - cx, py - cy) for px, py in pts)
        # rode floor: the anchor is within reach of every fix seen, so of their centroid too; the boat can
        # later lie anywhere within reach of the anchor -> up to 2 x reach from the centroid
        floor = 2 * self.reach if self.reach else 0.0
        self.alarm_radius = max(self.radius, floor) + self.cfg.tolerance_m
        self.outside_since = None
        self._reset_trend()
        self.raw.clear()
        basis = (f"rode floor 2 x {self.reach:.0f} m" if floor > self.radius
                 else f"max distance {self.radius:.1f} m")
        events.append(self._change(t, CONSERVATIVE,
            f"no lock - conservative circle: centroid of {len(pts)} fixes, {basis} "
            f"+ {self.cfg.tolerance_m:.0f} m = {self.alarm_radius:.1f} m; still trying to lock "
            f"({self.no_lock_reason})"))

    # ---- alarm ---------------------------------------------------------------------------------
    def _check_guard(self, t: float, x: float, y: float, events: list[Event]) -> None:
        """Coarse guard while ACQUIRING: no circle yet, but the boat can never legitimately be more than
        2 x reach from where the watch was armed."""
        if self.state != ACQUIRING or self.arm_point is None:
            return
        d = math.hypot(x - self.arm_point[0], y - self.arm_point[1])
        g = self._guard_radius()
        if d <= g:
            self.outside_since = None
            return
        if self.outside_since is None:
            self.outside_since = t
        elif t - self.outside_since >= self.cfg.debounce_s:
            self.alarm_at = t
            events.append(self._change(t, ALARM,
                f"WOULD ALARM: coarse guard - {d:.1f} m from where the watch was armed > {g:.1f} m "
                f"(2 x reach + tolerance) for {t - self.outside_since:.0f} s, before any circle was fitted"
                + self._ctx(t)))

    def _check_circle(self, t: float, x: float, y: float, events: list[Event]) -> None:
        ref = self.centre if self.centre is not None else self.arm_point   # a coarse-guard ALARM has no centre
        self.distance = math.hypot(x - ref[0], y - ref[1])
        if self.state == ALARM:
            return
        if self.distance <= self.alarm_radius:
            self.outside_since = None
            return
        if self.outside_since is None:
            self.outside_since = t
        elif t - self.outside_since >= self.cfg.debounce_s:
            self.alarm_at = t
            events.append(self._change(t, ALARM,
                f"WOULD ALARM: {self.distance:.1f} m from the {'anchor' if self.locked_at else 'conservative centre'}"
                f" > {self.alarm_radius:.1f} m for {t - self.outside_since:.0f} s" + self._ctx(t)))

    # ---- DRAG? trend ---------------------------------------------------------------------------
    def _reset_trend(self) -> None:
        self.trend.clear()
        self._tbin = None
        self.drag_suspected = False

    def _add_trend(self, t: float, events: list[Event]) -> None:
        """10-s averages of the distance from the frozen centre; evaluate the trend as each bin closes."""
        if self.centre is None:
            return
        idx = int((t - self.armed_at) // self.cfg.bin_s)
        if self._tbin is not None and self._tbin[0] != idx:
            _, st, sd, n = self._tbin
            self.trend.append((st / n, sd / n))
            self._tbin = None
            self._eval_trend(t, events)
        if self._tbin is None:
            self._tbin = [idx, 0.0, 0.0, 0]
        b = self._tbin
        b[1] += t; b[2] += self.distance; b[3] += 1

    @staticmethod
    def _slope(pts) -> float:
        n = len(pts)
        mt = sum(p[0] for p in pts) / n
        md = sum(p[1] for p in pts) / n
        stt = sum((p[0] - mt) ** 2 for p in pts)
        return sum((p[0] - mt) * (p[1] - md) for p in pts) / stt if stt > 0 else 0.0

    def _eval_trend(self, t: float, events: list[Event]) -> None:
        c = self.cfg
        while self.trend and self.trend[0][0] < t - c.trend_window_s:
            self.trend.popleft()
        if self.state == ALARM or not self.trend or t - self.trend[0][0] < c.trend_window_s - 2 * c.bin_s:
            return                                   # alarm already up, or not a full window yet
        if len(self.trend) < 0.6 * c.trend_window_s / c.bin_s:
            return                                   # too many gaps to judge a trend
        seg_s = c.trend_window_s / c.trend_segments
        segs = [[p for p in self.trend if t - (k + 1) * seg_s <= p[0] < t - k * seg_s]
                for k in range(c.trend_segments - 1, -1, -1)]          # oldest third first
        if any(len(sg) < 4 for sg in segs):
            return
        slope = self._slope(self.trend)
        sslopes = [self._slope(sg) for sg in segs]
        rslope = sslopes[-1]                                            # the latest third: still going?
        rise = slope * c.trend_window_s
        if not self.drag_suspected:
            if (slope >= c.trend_min_slope_mps and rise >= c.trend_min_rise_m
                    and min(sslopes) >= 0.5 * c.trend_min_slope_mps):
                self.drag_suspected = True
                events.append(Event(t, "DRAG", self.state, None,
                    f"DRAG? distance from the {'anchor' if self.locked_at else 'conservative centre'} has risen "
                    f"{rise:.0f} m in {c.trend_window_s / 60:.0f} min ({slope * 60:.1f} m/min) and is not levelling off; "
                    f"now {self.distance:.0f} m of {self.alarm_radius:.0f} m" + self._ctx(t)))
        elif slope <= 0.25 * c.trend_min_slope_mps and rslope <= 0.5 * c.trend_min_slope_mps:   # whole window flat
            self.drag_suspected = False
            events.append(Event(t, "INFO", self.state, None,
                f"DRAG? cleared - distance levelled off at {self.distance:.0f} m" + self._ctx(t)))

    # ---- power cut (10-02) --------------------------------------------------------------------
    def snapshot(self) -> dict | None:
        """What survives a power cut once a circle exists (LOCKED / CONSERVATIVE / ALARM): the circle and the
        state. NO age limit (10-02: re-fitting after an outage would quietly accept a slow drift); it is
        dropped only by DISARM or a fresh ARM. None while OFF / ACQUIRING (the caller re-arms fresh)."""
        if self.state not in (LOCKED, CONSERVATIVE, ALARM) or self.frame is None:
            return None
        ref = self.centre if self.centre is not None else self.arm_point   # coarse-guard ALARM: no centre
        lat, lon = self.frame.to_latlon(*ref)
        return {"state": self.state, "lat": lat, "lon": lon, "has_centre": self.centre is not None,
                "radius": self.radius or 0.0, "alarm_radius": self.alarm_radius or 0.0,
                "locked": self.locked_at is not None, "rode_ft": self.rode_ft or 0.0}

    def restore(self, t: float, snap: dict) -> list[Event]:
        """Power-up with a saved circle: carry on exactly where the watch was - same centre, same alarm circle, a
        latched ALARM stays latched. A drag during the outage alarms after the usual debounce."""
        self._reset()
        self.armed_at = t
        if snap.get("rode_ft"):
            self.rode_ft = float(snap["rode_ft"])
            self.reach = self.rode_ft * FT + self.cfg.bow_to_antenna_m
        self.frame = LocalFrame(snap["lat"], snap["lon"])      # origin = the saved centre (or arm point)
        self.arm_point = (0.0, 0.0)
        if snap["has_centre"]:
            self.centre = (0.0, 0.0)
            self.radius = snap["radius"]
            self.alarm_radius = snap["alarm_radius"]
        if snap["locked"]:
            self.locked_at = t
        if snap["state"] == ALARM:
            self.alarm_at = t
        self.state = OFF
        return [self._change(t, snap["state"],
                             f"RESTORED after power-up: {snap['state']}, circle {snap['alarm_radius']:.1f} m round "
                             f"{snap['lat']:.6f},{snap['lon']:.6f}")]

    # ---- report --------------------------------------------------------------------------------
    def status(self, t: float | None = None) -> Status:
        centre = self.centre
        radius = self.radius
        if centre is None and self.fit is not None:
            centre, radius = (self.fit.cx, self.fit.cy), self.fit.r
        clat = clon = None
        if centre is not None and self.frame is not None:
            clat, clon = self.frame.to_latlon(*centre)
        f = self.fit
        ref = t if t is not None else self.last_t
        return Status(
            state=self.state, armed_at=self.armed_at,
            elapsed_s=(ref - self.armed_at) if (ref is not None and self.armed_at is not None) else None,
            centre_lat=clat, centre_lon=clon, radius_m=radius, alarm_radius_m=self.alarm_radius,
            distance_m=self.distance, arc_span_deg=f.arc_span_deg if f else None, rms_m=f.rms if f else None,
            no_lock_reason=self.no_lock_reason if self.state in (ACQUIRING, CONSERVATIVE) else None,
            locked_at=self.locked_at, alarm_at=self.alarm_at, fixes_accepted=self.accepted,
            rejects=dict(self.rejects), gps_stale=self.stale, rode_ft=self.rode_ft, reach_m=self.reach,
            drag_suspected=self.drag_suspected,
            stw_kn=(v / KN if (v := self._fresh(self.stw, ref)) is not None else None) if ref is not None else None,
            wind_kn=(v / KN if (v := self._fresh(self.wind, ref)) is not None else None) if ref is not None else None)
