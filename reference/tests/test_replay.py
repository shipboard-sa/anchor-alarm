"""
Purpose: Tests for the anchor-watch replay tool and CLI - a generated GPX fixture (good swing + slow drag,
    2 s spacing like a handheld) locks and alarms; GPX 1.0 / no-namespace and CSV (epoch or ISO) parse;
    --arm-at; the `sim` subcommand writes a track.
Version: v1 (2026-09-30)
Scope: STANDALONE UTILITY - test suite for the anchor-box reference model (reference).
Architecture: tests/ of anchor-watch; anchorwatch/replay.py, anchorwatch/__main__.py, anchorwatch/sim.py.
    Files only under pytest's tmp_path. Offline.
Invocation: .venv/Scripts/python -m pytest   (from reference)
"""
import pytest

from anchorwatch.__main__ import main
from anchorwatch.geo import distance_m
from anchorwatch.replay import load_track, parse_time, replay
from anchorwatch.sim import Drag, simulate, write_gpx


@pytest.fixture
def drag_gpx(tmp_path):
    fx = simulate(duration_s=2.5 * 3600, swing="steady", seed=21, drag=Drag("slow", start_s=3600, speed_mps=0.08))
    return write_gpx(fx, tmp_path / "swing_drag.gpx", every_s=2.0), fx


def test_replay_of_generated_gpx_locks_then_alarms(drag_gpx):
    path, fx = drag_gpx
    fixes = load_track(path)
    assert len(fixes) == pytest.approx(len(fx) / 2, abs=2)
    lines, w = replay(fixes)
    text = "\n".join(lines)
    assert "ACQUIRING -> LOCKED" in text and "LOCKED -> ALARM" in text and "WOULD ALARM" in text
    lock = next(e for e in lines if "-> LOCKED" in e)
    alarm = next(e for e in lines if "-> ALARM" in e)
    assert lines.index(lock) < lines.index(alarm)
    assert w.status().locked_at - fixes[0][0] <= 900
    assert w.status().alarm_at - fixes[0][0] > 3600
    s = w.status()
    assert distance_m(s.centre_lat, s.centre_lon, fx[0].anchor_lat, fx[0].anchor_lon) < 5.0


def test_cli_replay_with_arm_at(drag_gpx, capsys):
    path, _ = drag_gpx
    assert main(["replay", str(path), "--arm-at", "300", "--every", "0"]) == 0
    out = capsys.readouterr().out
    assert "00:05:00  OFF -> ACQUIRING" in out and "-> LOCKED" in out


def test_gpx_10_and_no_namespace(tmp_path):
    for i, head in enumerate(['<gpx version="1.0" xmlns="http://www.topografix.com/GPX/1/0">', "<gpx>"]):
        p = tmp_path / f"t{i}.gpx"
        p.write_text(head + "<trk><trkseg>"
                     '<trkpt lat="29.95" lon="-90.06"><ele>1</ele><time>2026-10-01T12:00:05Z</time></trkpt>'
                     '<trkpt lat="29.9501" lon="-90.06"><time>2026-10-01T12:00:00.5+00:00</time></trkpt>'
                     '<trkpt lat="29.9502" lon="-90.06"></trkpt>'
                     "</trkseg></trk></gpx>", encoding="utf-8")
        fixes = load_track(p)
        assert [f[1] for f in fixes] == [29.9501, 29.95]      # sorted by time; the untimed point dropped


def test_csv_epoch_and_iso(tmp_path):
    p = tmp_path / "t.csv"
    p.write_text("t,lat,lon\n1790000001,29.95,-90.06\n2026-09-21T14:13:25Z,29.9501,-90.0601\n", encoding="utf-8")
    fixes = load_track(p)
    assert fixes[0][0] == 1790000001.0 and fixes[1][0] == 1790000005.0 == parse_time("2026-09-21T14:13:25Z")
    lines, w = replay(fixes)
    assert w.state == "ACQUIRING" and "end: state ACQUIRING" in "\n".join(lines)


def test_empty_track(tmp_path):
    p = tmp_path / "e.csv"
    p.write_text("t,lat,lon\n", encoding="utf-8")
    assert replay(load_track(p))[0] == ["no fixes in track"]


def test_cli_sim_writes_a_replayable_gpx(tmp_path, capsys):
    out = tmp_path / "s.gpx"
    assert main(["sim", "--out", str(out), "--hours", "0.5", "--every", "5", "--pattern", "large"]) == 0
    fixes = load_track(out)
    assert len(fixes) == 361
    assert main(["replay", str(out)]) == 0
    assert "-> LOCKED" in capsys.readouterr().out
