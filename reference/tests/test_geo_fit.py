"""
Purpose: Tests for the anchor-watch geometry and circle fit - projection round trip, distances, circle
    recovery on clean / noisy / partial arcs, arc span incl. wrap-around, degenerate inputs.
Version: v1 (2026-09-30)
Scope: STANDALONE UTILITY - test suite for the anchor-box reference model (reference).
Architecture: tests/ of anchor-watch; exercises anchorwatch/geo.py and anchorwatch/fit.py. Offline.
Invocation: .venv/Scripts/python -m pytest   (from reference)
"""
import math
import random

import pytest

from anchorwatch.fit import arc_span_deg, fit_circle, kasa, solve3
from anchorwatch.geo import LocalFrame, distance_m, offset


def arc(cx, cy, r, a0, a1, n, sigma=0.0, seed=1):
    rng = random.Random(seed)
    pts = []
    for i in range(n):
        a = math.radians(a0 + (a1 - a0) * i / (n - 1))
        pts.append((cx + r * math.cos(a) + rng.gauss(0, sigma), cy + r * math.sin(a) + rng.gauss(0, sigma)))
    return pts


def test_projection_round_trip_and_distance():
    f = LocalFrame(29.95, -90.06)
    for lat, lon in [(29.9504, -90.0597), (29.9490, -90.0611), (29.95, -90.06)]:
        x, y = f.to_xy(lat, lon)
        la, lo = f.to_latlon(x, y)
        assert la == pytest.approx(lat, abs=1e-10) and lo == pytest.approx(lon, abs=1e-10)
        assert math.hypot(x, y) == pytest.approx(distance_m(29.95, -90.06, lat, lon), rel=1e-4, abs=1e-3)


def test_projection_across_the_antimeridian():
    f = LocalFrame(-17.0, 179.9999)
    x, _ = f.to_xy(-17.0, -179.9999)
    assert x == pytest.approx(21.3, abs=0.2)          # 0.0002 deg of longitude east, not 360 deg west


def test_offset_goes_the_right_way():
    lat, lon = offset(29.95, -90.06, 90.0, 100.0)
    assert lat == pytest.approx(29.95, abs=1e-7) and lon > -90.06
    assert distance_m(29.95, -90.06, lat, lon) == pytest.approx(100.0, rel=1e-3)


@pytest.mark.parametrize("a0,a1", [(0, 360), (10, 70), (200, 300)])
def test_fit_recovers_clean_circle(a0, a1):
    f = fit_circle(arc(12.0, -7.0, 40.0, a0, a1, 50))
    assert f.cx == pytest.approx(12.0, abs=1e-6) and f.cy == pytest.approx(-7.0, abs=1e-6)
    assert f.r == pytest.approx(40.0, abs=1e-6) and f.rms < 1e-6
    assert f.arc_span_deg == pytest.approx(min(a1 - a0, 360) * (1 if a1 - a0 < 360 else 49 / 50), abs=0.5)


def test_fit_recovers_noisy_arc():
    f = fit_circle(arc(0.0, 0.0, 40.0, 30, 120, 90, sigma=0.5, seed=3))
    assert math.hypot(f.cx, f.cy) < 1.5
    assert f.r == pytest.approx(40.0, abs=1.5)
    assert 0.3 < f.rms < 0.8
    assert f.sagitta > 3 * f.rms


def test_arc_span_wraps_around_north():
    # bearings 350..10 deg (in math-angle terms) straddle the 0/360 seam: span is 20, not 340
    pts = arc(0, 0, 30, -10, 10, 21)
    assert arc_span_deg(pts, 0, 0) == pytest.approx(20.0, abs=1e-6)
    assert arc_span_deg(arc(0, 0, 30, 0, 270, 28), 0, 0) == pytest.approx(270.0, abs=1e-6)
    assert arc_span_deg([(1.0, 0.0)], 0, 0) == 0.0


def test_degenerate_inputs():
    assert fit_circle([(0, 0), (1, 1)]) is None
    assert kasa([(0, 0), (1, 1), (2, 2), (3, 3)]) is None       # collinear: no finite circle
    assert solve3([[1, 2, 3], [2, 4, 6], [1, 1, 1]], [1, 2, 3]) is None
