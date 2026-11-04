"""
Purpose: Tests for anchorwatch.boattime (anchor-box step 3): zone table vs the IANA tz database at real harbours,
    US daylight-saving edges, nautical zones, the 5-minute steadiness rule, the manual pin, GPS time acceptance,
    sun altitude vs astral, and the CLOCK face ('--:--' rules).
Version: v1 (2026-10-02)
Scope: STANDALONE UTILITY - reference-model tests (offline; tzdata + astral are test-only packages).
Architecture: reference/tests. The same cases are exported for the C port by
    tools/make_c_vectors.py.
Invocation: cd reference; .venv\\Scripts\\python -m pytest tests/test_boattime.py
"""
from __future__ import annotations

import datetime as dt
from zoneinfo import ZoneInfo

import pytest

from anchorwatch import boattime as bt

UTC = dt.timezone.utc

# (place, lat, lon, IANA zone) - harbours on the itinerary and both sides of every boundary
HARBOURS = [
    ("New Orleans", 29.95, -90.07, "America/Chicago"),
    ("Galveston", 29.30, -94.80, "America/Chicago"),
    ("Pensacola", 30.40, -87.21, "America/Chicago"),
    ("Panama City", 30.15, -85.66, "America/Chicago"),
    ("Mexico Beach", 29.95, -85.42, "America/Chicago"),
    ("Port St. Joe", 29.81, -85.30, "America/New_York"),
    ("Apalachicola", 29.72, -84.98, "America/New_York"),
    ("Tampa", 27.95, -82.46, "America/New_York"),
    ("Key West", 24.55, -81.80, "America/New_York"),
    ("Miami", 25.76, -80.19, "America/New_York"),
    ("Bimini", 25.73, -79.28, "America/Nassau"),
    ("Nassau", 25.05, -77.35, "America/Nassau"),
    ("George Town, Exumas", 23.51, -75.77, "America/Nassau"),
    ("Matthew Town, Inagua", 20.95, -73.67, "America/Nassau"),
    ("Grand Turk", 21.47, -71.14, "America/Grand_Turk"),
    ("Luperon", 19.90, -70.95, "America/Santo_Domingo"),
    ("San Juan", 18.47, -66.11, "America/Puerto_Rico"),
    ("St Thomas", 18.34, -64.93, "America/St_Thomas"),
    ("Tortola", 18.43, -64.62, "America/Tortola"),
    ("St Martin", 18.07, -63.05, "America/Lower_Princes"),
    ("Antigua", 17.12, -61.85, "America/Antigua"),
    ("Guadeloupe", 16.24, -61.53, "America/Guadeloupe"),
    ("Dominica", 15.30, -61.39, "America/Dominica"),
    ("Martinique", 14.60, -61.07, "America/Martinique"),
    ("St Lucia", 14.00, -61.00, "America/St_Lucia"),
    ("Barbados", 13.10, -59.61, "America/Barbados"),
    ("Bequia", 13.00, -61.24, "America/St_Vincent"),
    ("Grenada", 12.05, -61.75, "America/Grenada"),
    ("Trinidad", 10.68, -61.64, "America/Port_of_Spain"),
    ("Bonaire", 12.15, -68.27, "America/Kralendijk"),
    ("Curacao", 12.11, -68.93, "America/Curacao"),
    ("Aruba", 12.52, -70.03, "America/Aruba"),
]


def _instants():
    """Every hour from 3 h before to 3 h after each US change, 2026-2030, plus the 15th of each month."""
    out = []
    for y in range(2026, 2031):
        for m, n in ((3, 2), (11, 1)):
            day = bt._nth_sunday(y, m, n) * 86400
            out += [day + h * 3600 for h in range(0, 15)]
        out += [bt.days_from_civil(y, m, 15) * 86400 + 12 * 3600 for m in range(1, 13)]
    return out


@pytest.mark.parametrize("place,lat,lon,iana", HARBOURS, ids=[h[0] for h in HARBOURS])
def test_zone_matches_iana(place, lat, lon, iana):
    z = bt.zone_at(lat, lon)
    tz = ZoneInfo(iana)
    for t in _instants():
        want = int(dt.datetime.fromtimestamp(t, UTC).astimezone(tz).utcoffset().total_seconds() // 60)
        assert bt.offset_min(z, t) == want, (place, dt.datetime.fromtimestamp(t, UTC))


def test_dst_edges_2026_central():
    # 2026: DST starts Sun 8 Mar 02:00 CST = 08:00 UTC; ends Sun 1 Nov 02:00 CDT = 07:00 UTC
    start = bt.days_from_civil(2026, 3, 8) * 86400 + 8 * 3600
    end = bt.days_from_civil(2026, 11, 1) * 86400 + 7 * 3600
    assert bt.offset_min(bt.US_CENTRAL, start - 1) == -360
    assert bt.offset_min(bt.US_CENTRAL, start) == -300
    assert bt.offset_min(bt.US_CENTRAL, end - 1) == -300
    assert bt.offset_min(bt.US_CENTRAL, end) == -360


@pytest.mark.parametrize("lon,hours", [(0.0, 0), (-7.4, 0), (-7.6, -1), (7.6, 1), (-60.0, -4), (-89.9, -6),
                                       (-112.6, -8), (179.9, 12), (-179.9, -12)])
def test_nautical_zone(lon, hours):
    assert bt.nautical_zone(lon).std_min == hours * 60


def test_offshore_atlantic_is_nautical():
    z = bt.zone_at(32.0, -65.0)          # north of the table (Bermuda waters)
    assert z.zid == "N-4" and z.dst == bt.NO_DST


def test_zone_ids_round_trip():
    for zid in ("CT", "ET", "BS", "AT", "Z", "N-5", "N+3"):
        assert bt.zone_by_id(zid).zid == zid


# ---------------------------------------------------------------- steadiness + pin

def test_first_zone_immediate_then_steady_rule():
    c = bt.ZoneChooser()
    assert c.update(0, 30.0, -85.42).zid == "CT"                 # Mexico Beach
    assert c.update(10, 29.81, -85.30).zid == "CT"               # crossed the line: not yet
    assert c.update(10 + 299, 29.81, -85.30).zid == "CT"
    assert c.update(10 + 300, 29.81, -85.30).zid == "ET"         # held 5 min


def test_swinging_on_a_line_never_flips():
    c = bt.ZoneChooser()
    c.update(0, 29.9, -85.395)
    for t in range(1, 3600, 30):                                  # alternate sides every 30 s for an hour
        lon = -85.385 if (t // 30) % 2 else -85.395
        assert c.update(t, 29.9, lon).zid == "CT"


def test_pin_overrides_and_marks_manual():
    c = bt.ZoneChooser()
    c.update(0, 25.05, -77.35)
    c.pin(bt.zone_by_id("CT"))
    assert c.update(1, 25.05, -77.35).zid == "CT" and c.manual
    c.pin(None)
    assert c.update(2, 25.05, -77.35).zid == "BS" and not c.manual


def test_no_position_no_zone():
    c = bt.ZoneChooser()
    assert c.update(0, None, None) is None


# ---------------------------------------------------------------- GPS time acceptance

T0 = bt.days_from_civil(2026, 10, 2) * 86400_000 + 19 * 3600_000     # 2026-10-02 19:00 UTC, ms


def test_one_message_is_not_enough():
    s = bt.TimeSync()
    s.feed(T0, 1000)
    assert not s.valid()


def test_two_agreeing_messages_set_the_clock():
    s = bt.TimeSync()
    s.feed(T0, 1000)
    s.feed(T0 + 1000, 2000)
    assert s.valid() and s.now_ms(5000) == T0 + 4000


def test_disagreeing_pair_rejected():
    s = bt.TimeSync()
    s.feed(T0, 1000)
    s.feed(T0 + 60_000, 2000)
    assert not s.valid()


def test_non_gps_and_pre_2026_ignored():
    s = bt.TimeSync()
    s.feed(T0, 1000, kind=5)
    s.feed(T0 + 1000, 2000, kind=5)
    assert not s.valid()
    old = bt.days_from_civil(2019, 4, 7) * 86400_000                 # GPS week-rollover style garbage
    s.feed(old, 1000)
    s.feed(old + 1000, 2000)
    assert not s.valid()


def test_single_wild_message_does_not_move_clock():
    s = bt.TimeSync()
    s.feed(T0, 1000)
    s.feed(T0 + 1000, 2000)
    s.feed(T0 + 3_600_000, 3000)                                    # one bad message
    assert s.now_ms(3000) == T0 + 2000
    s.feed(T0 + 3000, 4000)                                         # good again
    assert s.now_ms(4000) == T0 + 3000


def test_real_jump_needs_two_agreeing_messages():
    s = bt.TimeSync()
    s.feed(T0, 1000)
    s.feed(T0 + 1000, 2000)
    s.feed(T0 + 600_000, 3000)
    s.feed(T0 + 601_000, 4000)
    assert s.now_ms(4000) == T0 + 601_000


# ---------------------------------------------------------------- sun / night

def test_sun_altitude_vs_astral():
    from astral import Observer
    from astral.sun import elevation
    pts = [(29.95, -90.07), (25.05, -77.35), (12.15, -68.27), (18.47, -66.11), (-33.9, 151.2), (60.0, 10.0)]
    for lat, lon in pts:
        for d in range(0, 366, 23):
            for h in range(0, 24, 3):
                t = (bt.days_from_civil(2026, 1, 1) + d) * 86400 + h * 3600 + 1234
                want = elevation(Observer(lat, lon), dt.datetime.fromtimestamp(t, UTC), with_refraction=False)
                assert abs(bt.sun_altitude_deg(t, lat, lon) - want) < 0.05, (lat, lon, d, h)


def test_night_flips_at_astral_sunset_new_orleans():
    from astral import Observer
    from astral.sun import sunset
    day = dt.date(2026, 10, 2)
    ss = int(sunset(Observer(29.95, -90.07), day).timestamp())       # UTC instant
    assert not bt.is_night(ss - 180, 29.95, -90.07)
    assert bt.is_night(ss + 180, 29.95, -90.07)


# ---------------------------------------------------------------- the CLOCK face

def test_face_dashes_until_time_and_zone():
    s, c = bt.TimeSync(), bt.ZoneChooser()
    assert bt.clock_face(s, c, 0, None, None).hh == -1
    s.feed(T0, 1000)
    s.feed(T0 + 1000, 2000)
    assert bt.clock_face(s, c, 2000, None, None).hh == -1            # time but no zone
    c.update(2, 29.95, -90.07)
    f = bt.clock_face(s, c, 2000, 29.95, -90.07)
    assert (f.hh, f.mm, f.manual, f.night) == (14, 0, False, False)  # 19:00 UTC = 14:00 CDT


def test_face_pinned_without_position_shows_time_not_night():
    s, c = bt.TimeSync(), bt.ZoneChooser()
    s.feed(T0, 1000)
    s.feed(T0 + 1000, 2000)
    c.pin(bt.zone_by_id("ET"))
    f = bt.clock_face(s, c, 2000, None, None)
    assert (f.hh, f.mm, f.manual, f.night) == (15, 0, True, False)
