"""
Purpose: Tests for AnchorWatch.snapshot() / restore() - the watch survives a power cut (10-02): the circle comes
    back unchanged with NO age limit, a drag during the outage alarms, a latched ALARM stays latched, and nothing is
    saved before a circle exists.
Version: v1 (2026-10-02)
Scope: STANDALONE UTILITY - reference-model tests (offline).
Architecture: reference/tests; uses sim.py swings. The C port is checked against the same restart
    in tools/make_watch_vectors.py (scenarios *_restart).
Invocation: cd reference; .venv\\Scripts\\python -m pytest tests/test_restore.py
"""
from __future__ import annotations

import pytest

from anchorwatch import sim
from anchorwatch.watch import ACQUIRING, ALARM, CONSERVATIVE, LOCKED, AnchorWatch


def feed(w, fixes, t_from=None, t_to=None):
    evs = []
    for f in fixes:
        if (t_from is None or f.t >= t_from) and (t_to is None or f.t < t_to):
            evs += w.update(f.t, f.lat, f.lon)
    return evs


def restarted(w, t_on):
    snap = w.snapshot()
    w2 = AnchorWatch()
    evs = w2.restore(t_on, snap)
    return w2, snap, evs


def test_nothing_saved_before_a_circle():
    w = AnchorWatch()
    w.arm(0, rode_ft=100)
    feed(w, sim.simulate(duration_s=300, seed=1), t_to=300)
    assert w.state == ACQUIRING and w.snapshot() is None


@pytest.mark.parametrize("outage_s", [60, 600, 48 * 3600])          # 48 h: there is NO age limit
def test_locked_circle_survives_unchanged(outage_s):
    fixes = sim.simulate(duration_s=3600, seed=21)
    w = AnchorWatch()
    w.arm(0, rode_ft=100)
    feed(w, fixes, t_to=2400)
    assert w.state == LOCKED
    before = w.status()
    w2, snap, evs = restarted(w, 2400 + outage_s)
    assert evs[0].state == LOCKED and w2.state == LOCKED
    after = w2.status()
    assert after.centre_lat == pytest.approx(before.centre_lat, abs=1e-9)
    assert after.centre_lon == pytest.approx(before.centre_lon, abs=1e-9)
    assert after.alarm_radius_m == before.alarm_radius_m
    # same swing continues (shifted in time past the outage): no false alarm
    more = [sim.SimFix(f.t + outage_s, f.lat, f.lon, 0, 0, 0, 0, False) for f in fixes if f.t >= 2400]
    feed(w2, more)
    assert w2.state == LOCKED


def test_drag_during_outage_alarms_on_power_up():
    fixes = sim.simulate(duration_s=7200, drag=sim.Drag("slow", 2500, 0.05), seed=22)
    w = AnchorWatch()
    w.arm(0, rode_ft=100)
    feed(w, fixes, t_to=2400)
    assert w.state == LOCKED
    w2, _, _ = restarted(w, 5000)                 # off 2400 .. 5000: dragged ~125 m meanwhile
    evs = feed(w2, fixes, t_from=5000)
    alarms = [e for e in evs if e.state == ALARM and e.kind == "STATE"]
    assert alarms and alarms[0].t <= 5000 + 25    # 20 s debounce after the first fix


def test_latched_alarm_stays_latched():
    fixes = sim.simulate(duration_s=5400, drag=sim.Drag("sudden", 3000), seed=23)
    w = AnchorWatch()
    w.arm(0, rode_ft=100)
    feed(w, fixes, t_to=4000)
    assert w.state == ALARM
    w2, _, evs = restarted(w, 4100)
    assert evs[0].state == ALARM and w2.state == ALARM
    assert w2.disarm(4200)[0].state == "OFF"


def test_conservative_restored_with_its_floor():
    fixes = sim.simulate(duration_s=3600, swing="none", seed=24)
    w = AnchorWatch()
    w.arm(0, rode_ft=100)
    feed(w, fixes, t_to=1200)
    assert w.state == CONSERVATIVE
    r = w.alarm_radius
    w2, _, _ = restarted(w, 1300)
    assert w2.state == CONSERVATIVE and w2.alarm_radius == r and w2.reach == w.reach


def test_disarm_clears_it():
    fixes = sim.simulate(duration_s=3600, seed=25)
    w = AnchorWatch()
    w.arm(0, rode_ft=100)
    feed(w, fixes, t_to=2400)
    w.disarm(2400)
    assert w.snapshot() is None
