"""
Purpose: Tests for the 10-02 additions to the anchor watch (./SPEC.md s.5): rode pick, the
    rode-floored CONSERVATIVE circle, the coarse guard while ACQUIRING, the DRAG? warning ('a wind stretch
    stops, a drag keeps going'), and water speed / wind as context only.
Version: v1 (2026-10-02)
Scope: STANDALONE UTILITY - test suite for the anchor-box reference model (reference).
Architecture: tests/ of anchor-watch; drives anchorwatch/watch.py with seeded anchorwatch/sim.py tracks
    (clock-free). These cases are the contract the anchor-box C port must also pass.
Invocation: .venv/Scripts/python -m pytest   (from reference)
"""
import math

import pytest

from anchorwatch.geo import offset
from anchorwatch.sim import Drag, Stretch, simulate
from anchorwatch.watch import ACQUIRING, ALARM, CONSERVATIVE, FT, KN, LOCKED, AnchorWatch, WatchConfig

R, TOL = 40.0, 15.0
RODE = 100.0                                   # ft: reach = 30.5 + 18.3 = 48.8 m >= R


def run(fixes, rode_ft=RODE, cfg=None):
    w = AnchorWatch(cfg)
    events = w.arm(fixes[0].t, rode_ft)
    for f in fixes:
        events += w.update(f.t, f.lat, f.lon)
    return w, events


def first(events, state):
    return next((e for e in events if e.kind == "STATE" and e.state == state), None)


def drags(events):
    return [e for e in events if e.kind == "DRAG"]


# ---- rode pick + reach -------------------------------------------------------------------------------

def test_arm_with_and_without_rode():
    w = AnchorWatch()
    ev = w.arm(0.0, 150)
    assert w.status().rode_ft == 150 and w.status().reach_m == pytest.approx(150 * FT + 60 * FT)
    assert "coarse guard" in ev[0].message
    ev = w.arm(1.0)                            # re-arm without a rode: no reach, no guard
    assert w.status().reach_m is None and "no coarse guard" in ev[0].message


# ---- CONSERVATIVE rode floor ------------------------------------------------------------------------

def test_conservative_circle_is_floored_at_twice_the_reach():
    fx = simulate(duration_s=1800, swing="none", seed=5, radius_m=R)
    w, ev = run(fx)
    cons = first(ev, CONSERVATIVE)
    assert cons is not None and "rode floor" in cons.message
    s = w.status()
    assert s.alarm_radius_m == pytest.approx(2 * s.reach_m + TOL)


def test_rode_floor_fixes_the_calm_then_180_false_alarm():
    """The pinned known limit in test_watch.py (no rode: false alarm) is gone once the rode is picked."""
    fx = simulate(duration_s=3600, swing="small-reversal", seed=10, radius_m=R)
    _, ev = run(fx)
    assert first(ev, CONSERVATIVE) is not None and first(ev, ALARM) is None


def test_locked_circle_ignores_the_rode():
    """Observed geometry + tolerance (10-02): a huge rode pick does not grow a LOCKED circle."""
    fx = simulate(duration_s=1800, swing="steady", seed=1, radius_m=R)
    w, _ = run(fx, rode_ft=400)
    s = w.status()
    assert s.state == LOCKED and s.alarm_radius_m < R + TOL + 6


# ---- coarse guard while ACQUIRING -------------------------------------------------------------------

def test_coarse_guard_alarms_on_a_drag_before_any_circle():
    fx = simulate(duration_s=1200, swing="steady", seed=21, radius_m=R,
                  drag=Drag("slow", start_s=60, speed_mps=0.3))
    w, ev = run(fx)
    alarm = first(ev, ALARM)
    assert alarm is not None and alarm.t < 900 and "coarse guard" in alarm.message
    assert first(ev, LOCKED) is None and first(ev, CONSERVATIVE) is None
    assert w.state == ALARM                     # latched like any alarm


def test_no_rode_no_coarse_guard():
    fx = simulate(duration_s=850, swing="steady", seed=21, radius_m=R,
                  drag=Drag("slow", start_s=60, speed_mps=0.3))
    w, ev = run(fx, rode_ft=None)
    assert first(ev, ALARM) is None and w.state == ACQUIRING


@pytest.mark.parametrize("pattern", ["steady", "large", "reversal"])
def test_coarse_guard_never_trips_on_a_normal_swing(pattern):
    fx = simulate(duration_s=900, swing=pattern, seed=22, radius_m=R)
    _, ev = run(fx)
    assert first(ev, ALARM) is None


# ---- DRAG? warning -----------------------------------------------------------------------------------

@pytest.mark.parametrize("speed,min_lead_s", [(0.02, 120), (0.03, 60)])
@pytest.mark.parametrize("seed", [1, 2, 3])
def test_slow_drag_warns_once_before_the_alarm(speed, min_lead_s, seed):
    fx = simulate(duration_s=3 * 3600, swing="steady", seed=seed, radius_m=R,
                  drag=Drag("slow", start_s=3600, speed_mps=speed))
    _, ev = run(fx)
    d, alarm = drags(ev), first(ev, ALARM)
    assert alarm is not None and len(d) == 1
    assert 3600 < d[0].t <= alarm.t - min_lead_s
    assert "not levelling off" in d[0].message


@pytest.mark.parametrize("extra_m", [5.0, 10.0, 14.0])
@pytest.mark.parametrize("seed", [1, 2])
def test_a_wind_stretch_that_stops_does_not_warn(extra_m, seed):
    fx = simulate(duration_s=3 * 3600, swing="steady", seed=seed, radius_m=R,
                  stretch=Stretch(start_s=3600, extra_m=extra_m, ramp_s=120, hold_s=3600))
    _, ev = run(fx)
    assert drags(ev) == [] and first(ev, ALARM) is None


@pytest.mark.parametrize("pattern,walk", [("steady", 0.0), ("steady", 2.0), ("reversal", 0.0), ("large", 0.0)])
def test_quiet_nights_never_warn(pattern, walk):
    fx = simulate(duration_s=8 * 3600, swing=pattern, seed=7, radius_m=R, walk_sigma_m=walk, walk_tau_s=300)
    _, ev = run(fx)
    assert drags(ev) == [] and first(ev, ALARM) is None


def test_drag_warning_clears_when_the_distance_levels_off():
    """Hand-fed: after LOCK the boat creeps out 0.03 m/s for 12 min from near the anchor, then holds."""
    fx = simulate(duration_s=1200, swing="steady", seed=13, radius_m=R, outlier_prob=0.0)
    w, _ = run(fx)
    assert w.state == LOCKED
    s = w.status()
    t, ev = fx[-1].t, []
    w.update(t + 1, *offset(s.centre_lat, s.centre_lon, 90.0, 5.0))   # (jump filter: one step at a time)
    t += 1
    for k in range(2400):                       # 900 s creeping out, then 1500 s holding
        t += 1
        ev += w.update(t, *offset(s.centre_lat, s.centre_lon, 90.0, 5.0 + 0.03 * min(k, 900)))
    kinds = [(e.kind, e.message.split(" ")[0]) for e in ev if e.kind in ("DRAG", "INFO")]
    assert kinds[0][0] == "DRAG"
    assert any(k == "INFO" and m == "DRAG?" for k, m in kinds)
    assert w.state == LOCKED and not w.status().drag_suspected


# ---- conditions = context only ------------------------------------------------------------------------

def test_conditions_are_quoted_but_never_change_the_circle():
    fx = simulate(duration_s=3 * 3600, swing="steady", seed=2, radius_m=R,
                  drag=Drag("slow", start_s=3600, speed_mps=0.03))
    w = AnchorWatch()
    ev = w.arm(fx[0].t, RODE)
    radii = set()
    for f in fx:
        w.context(f.t, stw_mps=0.0, wind_mps=28 * KN)
        ev += w.update(f.t, f.lat, f.lon)
        if w.state == LOCKED:
            radii.add(round(w.status().alarm_radius_m, 6))
    assert len(radii) == 1                     # frozen whatever the wind did
    d, alarm = drags(ev)[0], first(ev, ALARM)
    for e in (d, alarm):
        assert "wind 28 kn" in e.message and "water speed 0.0 kn (moving with the water?)" in e.message
    s = w.status(fx[-1].t)
    assert s.wind_kn == pytest.approx(28) and s.stw_kn == pytest.approx(0)


def test_stale_conditions_are_not_quoted():
    w = AnchorWatch()
    w.arm(0.0)
    w.context(0.0, stw_mps=1.0, wind_mps=5.0)
    assert w._ctx(30.0) != "" and w._ctx(61.0) == ""
    assert w.status(61.0).wind_kn is None
