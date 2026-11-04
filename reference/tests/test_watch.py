"""
Purpose: Behaviour tests for the anchor-watch state machine on synthetic swings - lock on a good swing
    (centre within ~5 m), CONSERVATIVE on little swing, no false alarm over 8 h of noise + outliers or a
    180 deg wind reversal, slow / sudden drag alarms within a bounded time, debounce, jump filter,
    disarm / re-arm, duplicates, GPS-stale warning.
Version: v1 (2026-09-30)
Scope: STANDALONE UTILITY - test suite for the anchor-box reference model (reference).
Architecture: tests/ of anchor-watch; anchorwatch/sim.py generates seeded tracks, anchorwatch/watch.py
    is driven with explicit timestamps (clock-free). Offline and deterministic.
Invocation: .venv/Scripts/python -m pytest   (from reference)
"""
import math

import pytest

from anchorwatch.geo import distance_m, offset
from anchorwatch.sim import Drag, Swing, simulate
from anchorwatch.watch import ACQUIRING, ALARM, CONSERVATIVE, LOCKED, OFF, AnchorWatch, WatchConfig

R, TOL = 40.0, 15.0


def run(fixes, cfg=None, watch=None):
    w = watch or AnchorWatch(cfg)
    events = [] if watch else w.arm(fixes[0].t)
    for f in fixes:
        events += w.update(f.t, f.lat, f.lon)
    return w, events


def first(events, state):
    return next((e for e in events if e.kind == "STATE" and e.state == state), None)


def anchor_error(w, fixes):
    s = w.status()
    return distance_m(s.centre_lat, s.centre_lon, fixes[0].anchor_lat, fixes[0].anchor_lon)


@pytest.mark.parametrize("pattern,seed", [("steady", 1), ("steady", 2), ("large", 3), ("large", 4)])
def test_good_swing_locks_near_the_true_anchor(pattern, seed):
    fx = simulate(duration_s=1800, swing=pattern, seed=seed, radius_m=R)
    w, ev = run(fx)
    lock = first(ev, LOCKED)
    assert lock is not None and 600 <= lock.t <= 900
    assert first(ev, CONSERVATIVE) is None
    assert anchor_error(w, fx) < 5.0
    assert w.status().radius_m == pytest.approx(R, abs=4.0)
    assert w.status().alarm_radius_m >= w.status().radius_m + TOL


@pytest.mark.parametrize("pattern", ["small", "none"])
def test_little_swing_does_not_lock_and_goes_conservative(pattern):
    fx = simulate(duration_s=1800, swing=pattern, seed=5, radius_m=R)
    w, ev = run(fx)
    assert first(ev, LOCKED) is None
    cons = first(ev, CONSERVATIVE)
    assert cons is not None and cons.t == pytest.approx(900, abs=2)
    assert "no lock" in cons.message
    s = w.status()
    assert s.state == CONSERVATIVE and s.no_lock_reason
    assert s.alarm_radius_m == pytest.approx(s.radius_m + TOL)


def test_conservative_switches_to_locked_when_the_swing_arrives():
    class LateSwing:          # hangs still for 20 min, then the swing builds to +-25 deg over 10 min
        def bearing(self, t):
            a = 2.0 + 23.0 * min(1.0, max(0.0, (t - 1200) / 600))
            return a * math.sin(2 * math.pi * t / 480)
    fx = simulate(duration_s=3600, swing=LateSwing(), seed=6, radius_m=R)
    w, ev = run(fx)
    assert [e.state for e in ev if e.kind == "STATE"][:3] == [ACQUIRING, CONSERVATIVE, LOCKED]
    assert first(ev, ALARM) is None
    assert anchor_error(w, fx) < 6.0


def test_eight_quiet_hours_with_noise_and_outliers_never_alarm():
    fx = simulate(duration_s=8 * 3600, swing="steady", seed=7, radius_m=R, outlier_prob=0.005)
    assert sum(f.outlier for f in fx) > 100
    w, ev = run(fx)
    assert first(ev, LOCKED) is not None and first(ev, ALARM) is None
    assert w.status().rejects["jump"] > 50


def test_eight_hours_of_slow_gps_wander_never_alarm():
    fx = simulate(duration_s=8 * 3600, swing="steady", seed=8, radius_m=R, walk_sigma_m=3.0, walk_tau_s=300)
    w, ev = run(fx)
    assert first(ev, LOCKED) is not None and first(ev, ALARM) is None


def test_wind_reversal_180_after_lock_does_not_alarm():
    fx = simulate(duration_s=4 * 3600, swing="reversal", seed=9, radius_m=R)
    w, ev = run(fx)
    assert first(ev, LOCKED).t < 1800 and first(ev, ALARM) is None
    assert w.state == LOCKED


def test_known_limit_conservative_circle_alarms_on_a_later_reversal():
    """Pinned known limit (README): with no lock the conservative circle is built from the little swing
    seen so far, so a later 180 deg shift takes the boat outside it - an announced, loud, safe failure."""
    fx = simulate(duration_s=3600, swing="small-reversal", seed=10, radius_m=R)
    _, ev = run(fx)
    assert first(ev, CONSERVATIVE) is not None and first(ev, ALARM) is not None


def crossing_time(fx):
    """First time the TRUE boat is further than R + tolerance from the ORIGINAL anchor."""
    a = (fx[0].anchor_lat, fx[0].anchor_lon)
    return next(f.t for f in fx if distance_m(f.boat_lat, f.boat_lon, *a) > R + TOL)


@pytest.mark.parametrize("mode,speed,bound_s", [("slow", 0.03, 300), ("slow", 0.05, 180),
                                                  ("slow", 0.1, 150), ("sudden", 0.05, 60)])
@pytest.mark.parametrize("seed", [11, 12])
def test_drag_alarms_within_bounded_time(mode, speed, bound_s, seed):
    fx = simulate(duration_s=3 * 3600, swing="steady", seed=seed, radius_m=R,
                  drag=Drag(mode, start_s=3600, speed_mps=speed))
    w, ev = run(fx)
    alarm = first(ev, ALARM)
    assert first(ev, LOCKED).t < 3600
    assert alarm is not None and alarm.t > 3600
    assert alarm.t - crossing_time(fx) <= bound_s
    assert "WOULD ALARM" in alarm.message
    # latched: stays in ALARM however the track continues
    assert w.state == ALARM and w.status().alarm_at == alarm.t


def locked_watch(cfg=None):
    fx = simulate(duration_s=1200, swing="steady", seed=13, radius_m=R, outlier_prob=0.0)
    w, ev = run(fx, cfg)
    assert w.state == LOCKED
    return w, fx


def outside_point(w, extra_m):
    s = w.status()
    return offset(s.centre_lat, s.centre_lon, 0.0, s.alarm_radius_m + extra_m)


def inside_point(w):
    s = w.status()
    return offset(s.centre_lat, s.centre_lon, 0.0, s.radius_m)


def test_debounce_single_excursions_do_not_alarm():
    w, fx = locked_watch(WatchConfig(max_speed_mps=1000.0))     # jump filter off: test the debounce alone
    t = fx[-1].t
    for k in range(30):                  # a 30 m excursion outside the circle once a minute, for 30 min
        for s in range(60):
            t += 1
            lat, lon = outside_point(w, 30.0) if s == 0 else inside_point(w)
            assert w.update(t, lat, lon) == []
    for s in range(19):                  # 19 s outside is still not 20 s
        t += 1
        assert w.update(t, *outside_point(w, 5.0)) == []
    t += 1
    ev = w.update(t, *inside_point(w))   # back inside resets the timer
    assert ev == [] and w.state == LOCKED
    for s in range(21):
        t += 1
        ev = w.update(t, *outside_point(w, 5.0))
    assert w.state == ALARM and ev and ev[0].state == ALARM


def test_jump_filter_rejects_bursts_but_never_hides_a_real_move():
    w, fx = locked_watch()
    t = fx[-1].t
    far = outside_point(w, 60.0)
    for _ in range(5):                   # a short burst of wild fixes: rejected, no alarm
        t += 1
        w.update(t, *far)
    assert w.status().rejects["jump"] >= 5 and w.state == LOCKED
    t += 1
    w.update(t, fx[-1].lat, fx[-1].lon)  # back where it was
    events = []
    for _ in range(60):                  # the boat really is over there now: accepted after 10, then alarm
        t += 1
        events += w.update(t, *far)
    assert w.status().rejects["filter_reset"] >= 1
    assert any(e.kind == "WARN" and "jumps in a row" in e.message for e in events)
    assert w.state == ALARM


def test_disarm_and_rearm():
    fx = simulate(duration_s=3 * 3600, swing="steady", seed=14, radius_m=R, drag=Drag("sudden", start_s=3600))
    w, ev = run(fx)
    assert w.state == ALARM
    t = fx[-1].t
    ev = w.disarm(t)
    assert ev[0].state == OFF and ev[0].prev == ALARM and w.state == OFF
    assert w.update(t + 1, fx[-1].lat, fx[-1].lon) == [] and w.status().fixes_accepted == 0
    assert w.disarm(t + 2) == []
    # re-arm where the boat now lies: a fresh watch that locks on the new swing
    fx2 = simulate(duration_s=1800, swing="steady", seed=15, radius_m=R, t0=t + 10)
    ev = w.arm(fx2[0].t)
    assert ev[0].state == ACQUIRING and w.status().alarm_at is None
    _, ev = run(fx2, watch=w)
    assert first(ev, LOCKED) is not None and w.state == LOCKED


def test_rearm_from_alarm_clears_the_latch():
    w, fx = locked_watch()
    w.alarm_at, w.state = 1.0, ALARM
    ev = w.arm(fx[-1].t + 1)
    assert ev[0].prev == ALARM and ev[0].state == ACQUIRING and w.status().alarm_at is None


def test_duplicates_and_out_of_order_fixes_are_rejected():
    w = AnchorWatch()
    w.arm(0.0)
    w.update(1.0, 29.95, -90.06)
    w.update(1.0, 29.95, -90.06)
    w.update(0.5, 29.95, -90.06)
    w.update(2.0, 29.95, -90.06)         # newer but identical position: accepted, not fed to the fit
    s = w.status()
    assert s.rejects["duplicate"] == 2 and s.rejects["repeat"] == 1 and s.fixes_accepted == 2


def test_gps_stale_warns_once_then_recovers():
    w = AnchorWatch()
    w.arm(0.0)
    w.update(1.0, 29.95, -90.06)
    assert w.tick(20.0) == []
    ev = w.tick(40.0)
    assert ev and ev[0].kind == "WARN" and "stale" in ev[0].message and w.status().gps_stale
    assert w.tick(50.0) == []
    ev = w.update(51.0, 29.95001, -90.06)
    assert any(e.kind == "INFO" for e in ev) and not w.status().gps_stale


def test_off_ignores_everything():
    w = AnchorWatch()
    assert w.update(1.0, 29.95, -90.06) == [] and w.tick(100.0) == []
    assert w.status().state == OFF
