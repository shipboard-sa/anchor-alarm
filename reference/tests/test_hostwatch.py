"""
Purpose: Tests for the 'monitoring host down' watch (anchorwatch/hostwatch.py, ANC-25): Pi down / network down
    attribution, Cerbo silent / bus silent attribution, 30 s release, no flapping, unidentified Cerbo = no lamp.
Version: v1 (2026-10-03)
Scope: STANDALONE UTILITY - reference-model tests (offline).
Architecture: reference/tests. The C port (main/hostwatch.c) replays the same scenarios through
    tools/make_hostwatch_vectors.py -> test_host/test_hostwatch.c.
Invocation: cd reference; .venv\\Scripts\\python -m pytest tests/test_hostwatch.py
"""
from __future__ import annotations

from anchorwatch.hostwatch import DEGRADED, DOWN, NET_DOWN, OK, UNKNOWN, CerboWatch, PiWatch, do1, screen_lines

POLL = 10


def run_pi(w, t0, t1, pi_ok, router_ok=True):
    ev = []
    for t in range(t0, t1, POLL):
        r = router_ok if w.wants_router(t) else None
        ev += [(t, e) for e in w.update(t, pi_ok, r)]
    return ev


def test_pi_healthy_never_asserts():
    w = PiWatch()
    assert run_pi(w, 0, 3600, True) == [] and w.state == OK and not w.asserted


def test_pi_down_asserts_after_60s_with_router_up():
    w = PiWatch()
    ev = run_pi(w, 0, 200, False)
    assert ev == [(60, "PI_DOWN")] and w.asserted and w.state == DOWN
    assert screen_lines(w, CerboWatch()) == ["Pi / SignalK not answering"]


def test_pi_short_outage_does_not_assert():
    w = PiWatch()
    run_pi(w, 0, 100, True)
    ev = run_pi(w, 100, 150, False)            # 50 s bad: 100..140
    assert w.state == DEGRADED and ev == []
    assert run_pi(w, 150, 300, True) == [] and not w.asserted


def test_router_down_means_network_down_no_lamp():
    w = PiWatch()
    ev = run_pi(w, 0, 300, False, router_ok=False)
    assert ev == [(60, "PI_NET_DOWN")] and not w.asserted and w.state == NET_DOWN
    assert screen_lines(w, CerboWatch()) == ["Network down - router not answering"]
    # the network comes back but the Pi is still dead -> now it is the Pi
    ev = run_pi(w, 300, 400, False, router_ok=True)
    assert ev == [(300, "PI_DOWN")] and w.asserted


def test_pi_release_needs_30s_healthy_and_no_flap():
    w = PiWatch()
    run_pi(w, 0, 100, False)
    assert w.asserted
    ev = run_pi(w, 100, 120, True)             # 100, 110 good: 10 s
    ev += run_pi(w, 120, 130, False)           # one bad poll resets the recovery
    ev += run_pi(w, 130, 160, True)            # 130..150 good: 20 s
    assert ev == [] and w.asserted
    ev = run_pi(w, 160, 200, True)             # 30 s since 130 -> release at 160
    assert ev == [(160, "PI_RELEASED")] and not w.asserted and w.state == OK


def test_pi_lamp_held_when_router_dies_later():
    w = PiWatch()
    run_pi(w, 0, 100, False)
    assert w.asserted
    run_pi(w, 100, 400, False, router_ok=False)
    assert w.asserted and w.state == DOWN      # never released by attribution alone


def run_cerbo(w, t0, t1, cerbo_period, other_period=1, known=True, start_cerbo=None, start_other=None):
    """frames every cerbo_period s (None = silent) from the Cerbo, every other_period (None = silent) from others"""
    ev = []
    for t in range(t0, t1):
        lc = start_cerbo
        if cerbo_period:
            lc = t - (t % cerbo_period)
        lo = start_other
        if other_period:
            lo = t - (t % other_period)
        ev += [(t, e) for e in w.update(t, known, lc, lo)]
    return ev, lc, lo


def test_cerbo_heartbeat_only_never_asserts():
    w = CerboWatch()
    ev, _, _ = run_cerbo(w, 0, 3600, 60)
    assert ev == [] and w.state == OK


def test_cerbo_silent_asserts_at_180s():
    w = CerboWatch()
    _, lc, _ = run_cerbo(w, 0, 100, 10)        # last frame at 90
    ev, _, _ = run_cerbo(w, 100, 400, None, start_cerbo=lc)
    assert ev == [(270, "CERBO_DOWN")] and w.asserted
    assert screen_lines(PiWatch(), w) == ["Cerbo silent on N2K"]


def test_whole_bus_silent_is_not_the_cerbo():
    w = CerboWatch()
    _, lc, lo = run_cerbo(w, 0, 100, 10)
    ev, _, _ = run_cerbo(w, 100, 400, None, other_period=None, start_cerbo=lc, start_other=lo)
    assert ev == [(270, "CERBO_BUS_SILENT")] and not w.asserted and w.state == NET_DOWN
    assert screen_lines(PiWatch(), w) == ["N2K bus silent"]


def test_cerbo_release_after_30s_back():
    w = CerboWatch()
    _, lc, _ = run_cerbo(w, 0, 100, 10)
    run_cerbo(w, 100, 400, None, start_cerbo=lc)
    assert w.asserted
    ev, _, _ = run_cerbo(w, 400, 500, 5)       # frames again from 400
    assert ev == [(430, "CERBO_RELEASED")] and not w.asserted


def test_unidentified_cerbo_never_asserts():
    w = CerboWatch()
    ev, _, _ = run_cerbo(w, 0, 1000, None, known=False)
    assert ev == [] and w.state == UNKNOWN and not w.asserted


def test_identified_late_counts_from_identification():
    w = CerboWatch()
    run_cerbo(w, 0, 500, None, known=False)
    ev, _, _ = run_cerbo(w, 500, 800, None)    # known from 500, nothing heard since
    assert ev == [(680, "CERBO_DOWN")]


def test_do1_is_or_of_both():
    p, c = PiWatch(), CerboWatch()
    assert not do1(p, c)
    p.asserted = True
    assert do1(p, c)
    p.asserted, c.asserted = False, True
    assert do1(p, c)
