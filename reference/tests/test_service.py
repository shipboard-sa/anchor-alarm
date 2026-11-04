"""
Purpose: Tests for the anchor-watch DRY-RUN service - refuses non-loopback SignalK (the boat) unless
    --allow-remote, parses navigation.position deltas, logs WOULD ALARM, and reads from an in-process
    loopback WebSocket / REST stand-in for SignalK (subscribes, never publishes).
Version: v2 (2026-10-01) - v2: --follow-mode test (D-092). v1 (2026-09-30)
Scope: STANDALONE UTILITY - test suite for the anchor-box reference model (reference).
Architecture: tests/ of anchor-watch; anchorwatch/service.py. The "SignalK" here is a websockets /
    http.server stub bound to 127.0.0.1 inside the test - nothing leaves the machine.
Invocation: .venv/Scripts/python -m pytest   (from reference)
"""
import asyncio
import json
import logging
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer

import pytest

from anchorwatch import service
from anchorwatch.service import DryRun, check_target, is_loopback, positions_from_delta, stream_url
from anchorwatch.sim import Drag, simulate
from anchorwatch.watch import WatchConfig


@pytest.mark.parametrize("url,ok", [("http://127.0.0.1:3100", True), ("http://localhost:3000", True),
                                    ("ws://[::1]:3000", True), ("http://192.168.1.10:3000", False),
                                    ("http://your-pi-hostname.local:3000", False), ("http://10.0.0.5", False)])
def test_loopback_only_by_default(url, ok):
    assert is_loopback(url) is ok
    if ok:
        check_target(url, False)
    else:
        with pytest.raises(SystemExit):
            check_target(url, False)
        check_target(url, True)          # explicit --allow-remote


def test_stream_url():
    assert stream_url("http://127.0.0.1:3100") == "ws://127.0.0.1:3100/signalk/v1/stream?subscribe=none"
    assert stream_url("https://h/") == "wss://h/signalk/v1/stream?subscribe=none"


def test_positions_from_delta():
    msg = {"context": "vessels.self", "updates": [{"values": [
        {"path": "navigation.position", "value": {"latitude": 29.95, "longitude": -90.06}},
        {"path": "navigation.speedOverGround", "value": 0.1},
        {"path": "navigation.position", "value": {"latitude": None, "longitude": 1}}]}]}
    assert positions_from_delta(msg) == [(29.95, -90.06)]
    assert positions_from_delta({"name": "hello"}) == []


def test_dry_run_logs_lock_and_would_alarm(caplog):
    fx = simulate(duration_s=2 * 3600, swing="steady", seed=31, drag=Drag("sudden", start_s=3000))
    dry = DryRun(WatchConfig(), arm=True, status_every=600)
    with caplog.at_level(logging.INFO, logger="anchorwatch"):
        for f in fx:
            dry.on_fix(f.lat, f.lon, t=f.t)
    text = caplog.text
    assert "arming (dry run)" in text and "ACQUIRING -> LOCKED" in text and "WOULD ALARM" in text
    assert any(r.levelno == logging.CRITICAL for r in caplog.records)


def delta(lat, lon):
    return json.dumps({"context": "vessels.self", "updates": [{"values": [
        {"path": "navigation.position", "value": {"latitude": lat, "longitude": lon}}]}]})


def test_websocket_subscribes_and_reads_on_loopback():
    import websockets
    received = []

    async def handler(ws):
        await ws.send(json.dumps({"name": "signalk-server", "version": "2.x"}))   # hello
        received.append(json.loads(await ws.recv()))
        for i in range(5):
            await ws.send(delta(29.95 + i * 1e-6, -90.06))
        await asyncio.sleep(1.0)

    async def go():
        async with websockets.serve(handler, "127.0.0.1", 0) as srv:
            port = srv.sockets[0].getsockname()[1]
            dry = DryRun(WatchConfig(), arm=True, status_every=0)
            await asyncio.wait_for(service.run_ws(f"http://127.0.0.1:{port}", dry, max_fixes=5), 10)
            return dry

    dry = asyncio.run(go())
    assert received == [service.subscribe_message()]          # a subscribe, nothing else sent
    assert dry.fixes == 5 and dry.watch.state == "ACQUIRING"


def test_rest_poll_on_loopback():
    hits = []

    class H(BaseHTTPRequestHandler):
        def do_GET(self):
            hits.append((self.command, self.path))
            n = len(hits)
            body = json.dumps({"value": {"latitude": 29.95 + n * 1e-6, "longitude": -90.06},
                               "timestamp": f"2026-10-01T12:00:{n:02d}Z"}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    srv = HTTPServer(("127.0.0.1", 0), H)
    th = threading.Thread(target=srv.serve_forever, daemon=True)
    th.start()
    try:
        dry = DryRun(WatchConfig(), arm=True, status_every=0)
        service.run_poll(f"http://127.0.0.1:{srv.server_address[1]}", dry, interval=0.01, max_fixes=3)
    finally:
        srv.shutdown()
    assert dry.fixes == 3
    assert all(m == "GET" and p == "/signalk/v1/api/vessels/self/navigation/position" for m, p in hits)


# --- --follow-mode: the boat mode is the arm switch (D-092) ---------------------------------------------

def test_follow_mode_arms_on_anchor_and_disarms_on_any_other_mode():
    from anchorwatch.service import DryRun, modes_from_delta, subscribe_message
    from anchorwatch.watch import WatchConfig
    d = DryRun(WatchConfig(), arm=False, status_every=0, clock=lambda: 0.0)
    assert d.watch.state == "OFF"
    d.on_mode("MOTOR", 1.0)
    assert d.watch.state == "OFF"
    d.on_mode("ANCHOR", 2.0)
    assert d.watch.state != "OFF"
    d.on_mode("ANCHOR", 3.0)                     # repeated value: no re-arm, no change
    assert d.watch.state != "OFF"
    d.on_mode("MOTOR", 4.0)
    assert d.watch.state == "OFF"
    msg = {"updates": [{"values": [{"path": "automation.mode", "value": "ANCHOR"},
                                   {"path": "navigation.position", "value": {"latitude": 1, "longitude": 2}}]}]}
    assert modes_from_delta(msg) == ["ANCHOR"]
    paths = [s["path"] for s in subscribe_message(True)["subscribe"]]
    assert paths == ["navigation.position", "navigation.speedThroughWater", "environment.wind.speedApparent",
                     "automation.mode"]


def test_context_values_from_delta_and_rode_at_arm():
    from anchorwatch.service import DryRun, numbers_from_delta
    from anchorwatch.watch import KN, WatchConfig
    msg = {"updates": [{"values": [{"path": "navigation.speedThroughWater", "value": 0.5},
                                   {"path": "environment.wind.speedApparent", "value": True},
                                   {"path": "environment.wind.speedApparent", "value": 10.0}]}]}
    assert numbers_from_delta(msg, "navigation.speedThroughWater") == [0.5]
    assert numbers_from_delta(msg, "environment.wind.speedApparent") == [10.0]   # a bool is not a number
    d = DryRun(WatchConfig(), arm=True, status_every=0, clock=lambda: 0.0, rode_ft=150)
    d.on_context(wind_mps=10 * KN, t=0.0)
    d.on_fix(29.95, -90.06, t=1.0)
    s = d.watch.status(1.0)
    assert s.rode_ft == 150 and s.wind_kn == pytest.approx(10)
