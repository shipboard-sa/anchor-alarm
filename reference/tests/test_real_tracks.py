"""
Purpose: Replays of REAL recorded tracks (reference/data/) through the watch - a 2021 Chesapeake anchorage
    (33.5 h, Garmin Explore, ~2 min spacing) must never alarm, and must lock once the bins match its log rate.
Version: v1 (2026-10-02)
Scope: STANDALONE UTILITY - reference-model tests (offline).
Architecture: reference/tests; reads data/explore/*.gpx via anchorwatch.replay.load_track.
    ANQ-03 (handheld-GPS test data). The box itself sees ~1 Hz GPS; these logs are sparse, so the lock test
    scales bin_s / fit_window_s to the log rate - a 2-min log can never fill 12 bins of 10 s in 15 min.
Invocation: cd reference; .venv\\Scripts\\python -m pytest tests/test_real_tracks.py
"""
from __future__ import annotations

from pathlib import Path

import pytest

from anchorwatch.replay import load_track
from anchorwatch.watch import ALARM, LOCKED, AnchorWatch, WatchConfig

DATA = Path(__file__).resolve().parent.parent / "data" / "explore"
CHESAPEAKE = DATA / "2021-10-08_chesapeake_anchorage.gpx"


def _run(cfg: WatchConfig | None = None):
    fixes = load_track(CHESAPEAKE)
    w = AnchorWatch(cfg)
    events = list(w.arm(fixes[0][0]))
    for t, lat, lon, *_ in fixes:
        events += w.update(t, lat, lon)
    return fixes, w, events


@pytest.mark.skipif(not CHESAPEAKE.exists(), reason="real track not present")
def test_chesapeake_default_config_never_alarms():
    fixes, w, events = _run()
    assert len(fixes) == 1006
    assert fixes[-1][0] - fixes[0][0] > 33 * 3600
    assert not any(e.state == ALARM for e in events)


@pytest.mark.skipif(not CHESAPEAKE.exists(), reason="real track not present")
def test_chesapeake_locks_when_bins_match_log_rate():
    _, w, events = _run(WatchConfig(bin_s=120.0, fit_window_s=3 * 3600.0))
    assert not any(e.state == ALARM for e in events)
    lock = next(e for e in events if e.state == LOCKED)
    assert lock.t - events[0].t < 2 * 3600          # 1.7 h on 10-02
    s = w.status()
    assert 15.0 < s.radius_m < 26.0                  # 20.7 m on 10-02
