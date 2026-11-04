"""
Purpose: Tests for the v4 HEADING LOCK (narrow swing, 10-08) - the bow rays (position + heading) cross at the
    anchor; it must lock where the circle fit cannot, place the anchor within the compass-error bound, never
    false-alarm on a tide reversal (SPEC 5.10), still alarm on a real drag, and change nothing without heading.
Version: v1 (2026-10-08)
Scope: STANDALONE UTILITY - reference-model tests (offline).
Architecture: reference/tests; tracks from data/synth/make_drift_scenario_1008.build() (owner's 10-08
    scenario) plus tide-reversal / drag tails built here in the same local frame.
Invocation: cd reference; .venv\\Scripts\\python -m pytest tests/test_heading_lock.py
"""
from __future__ import annotations

import math
import random
import sys
from pathlib import Path

import pytest

from anchorwatch.watch import ALARM, CONSERVATIVE, LOCKED, AnchorWatch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "data" / "synth"))
import make_drift_scenario_1008 as scenario  # noqa: E402

M_LAT = 111_320.0
M_LON = 111_320.0 * math.cos(math.radians(scenario.ANCHOR_LAT))


def _xy(lat, lon):
    return (lon - scenario.ANCHOR_LON) * M_LON, (lat - scenario.ANCHOR_LAT) * M_LAT


def _fix(t, x, y, anchor, bias, rng):
    hdg = (math.degrees(math.atan2(anchor[0] - x, anchor[1] - y)) + bias + rng.gauss(0, 2.0)) % 360
    x += rng.gauss(0, scenario.NOISE_M); y += rng.gauss(0, scenario.NOISE_M)
    return (t, scenario.ANCHOR_LAT + y / M_LAT, scenario.ANCHOR_LON + x / M_LON, hdg)


def _run(fixes, heading=True, rode_ft=150):
    w = AnchorWatch()
    ev = list(w.arm(fixes[0][0], rode_ft=rode_ft))
    for t, la, lo, h in fixes:
        ev += w.update(t, la, lo, h if heading else None)
    return w, ev


def _set_only(seed=1, bias=8.0):
    fixes, _, anc = scenario.build(seed, hdg_bias=bias)
    return fixes[:900], anc[:900]


def test_lee_scenario_locks_on_heading_where_the_circle_cannot():
    fixes, _, _ = scenario.build(1)
    w, ev = _run(fixes)
    lock = next(e for e in ev if e.state == LOCKED)
    assert "LOCK (heading)" in lock.message
    assert lock.t - fixes[0][0] <= 15 * 60
    assert w.alarm_radius < 80.0                      # 66 m on 10-08, vs 143 m CONSERVATIVE without heading
    assert not any(e.state == ALARM for e in ev)      # 2 x 20 ft drag < compass margin + 15 m tolerance


def test_without_heading_nothing_changes():
    fixes, _, _ = scenario.build(1)
    _, ev = _run(fixes, heading=False)
    assert not any(e.state == LOCKED for e in ev)
    assert any(e.state == CONSERVATIVE for e in ev)


@pytest.mark.parametrize("bias", [-10.0, -5.0, 0.0, 5.0, 10.0])
@pytest.mark.parametrize("seed", [1, 2, 3])
def test_anchor_within_compass_bound(seed, bias):
    fixes, anc = _set_only(seed, bias)
    w, ev = _run(fixes)
    assert any(e.state == LOCKED for e in ev), [e.message for e in ev]
    ax, ay = w.frame.to_latlon(*w.centre)
    ex, ey = _xy(ax, ay)
    r = 135 * scenario.FT
    err = math.hypot(ex - anc[0][0], ey - anc[0][1])
    assert err <= 2 * r * math.sin(math.radians(abs(bias))) + 4.0, err


@pytest.mark.parametrize("bias", [-10.0, 0.0, 10.0])
def test_tide_reversal_never_alarms(bias):
    """After the lock, she swings right round the anchor (tide turns) at the same distance - SPEC 5.10."""
    fixes, anc = _set_only(1, bias)
    rng = random.Random(7)
    t0 = fixes[-1][0]
    r = 140 * scenario.FT                                    # the longest slack-free distance in the set
    tail = []
    for i in range(1, 1801):                            # 30 min, 180 -> 360 deg (north of the anchor)
        b = math.radians(180 + 180 * i / 1800)
        tail.append(_fix(t0 + i, r * math.sin(b), r * math.cos(b), anc[0], bias, rng))
    tail += [_fix(tail[-1][0] + i, 0.0, r, anc[0], bias, rng) for i in range(1, 1201)]   # 20 min held there
    w, ev = _run(fixes + tail)
    assert any(e.state == LOCKED for e in ev)
    assert not any(e.state == ALARM for e in ev), [e.message for e in ev if e.kind != "INFO"]


def test_real_drag_alarms_after_heading_lock():
    fixes, anc = _set_only(1, 8.0)
    rng = random.Random(9)
    last = fixes[-1]
    x0, y0 = _xy(last[1], last[2])
    tail = []
    for i in range(1, 901):                             # drags south 0.1 m/s for 15 min = 90 m
        a = (anc[0][0], anc[0][1] - 0.1 * i)
        tail.append(_fix(last[0] + i, x0, y0 - 0.1 * i, a, 8.0, rng))
    _, ev = _run(fixes + tail)
    assert any(e.state == LOCKED for e in ev)
    alarm = next(e for e in ev if e.state == ALARM)
    assert alarm.t - last[0] < 600                      # within 10 min (60 m of drag)


def test_heading_pointing_away_never_locks():
    """A heading 180 deg out (e.g. a reversed sensor) puts the 'anchor' behind the bow - refuse it."""
    fixes, _ = _set_only(1, 180.0)
    _, ev = _run(fixes)
    assert not any("LOCK (heading)" in e.message for e in ev)
