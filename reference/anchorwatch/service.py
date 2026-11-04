"""
Purpose: DRY-RUN live anchor watch - reads navigation.position from a SignalK server (WebSocket
    subscription, or REST polling with --poll), feeds the watch, and LOGS state changes and
    "WOULD ALARM" lines. Publishes nothing, writes nothing anywhere except its own log / stdout.
Version: v3 (2026-10-02) - v3: --rode-ft (rode pick at ARM); subscribes water speed + apparent wind as CONTEXT
    (quoted in DRAG? / WOULD ALARM lines, never change the circle - 10-02); logs DRAG? warnings. v2 (2026-10-01) - v2: --follow-mode arms on the engine's boat mode ANCHOR and disarms on any other mode
    (`automation.mode`, D-092: the ANCHOR tap on the phone / cockpit tablet arms the watch; only a tap disarms it).
    v1 (2026-09-30).
Scope: STANDALONE UTILITY - anchor-watch bench tool (dry run only; drives no relay, horn or SignalK path).
Architecture: anchor-watch. SignalK (read only) -> service.DryRun -> watch.AnchorWatch -> log lines.
    Default target is the laptop bench SignalK at 127.0.0.1:3100 (your-pi-hostname's stacks/signalk/bench); non-loopback
    hosts are REFUSED unless --allow-remote (so it cannot be pointed at the boat by accident).
    Fix time = local receive time (monotonic enough for the 20 s debounce; the GPS clock is not trusted).
    Optional read token from the env var SIGNALK_TOKEN (never a file in git).
Invocation: python -m anchorwatch run [--signalk http://127.0.0.1:3100] [--arm | --follow-mode] [--rode-ft 150] [--tolerance 15]
    [--debounce 20] [--poll] [--status-every 60] [--log FILE] [--allow-remote]   (from reference)
    Without --arm it only reports that it sees positions (state OFF). Ctrl+C to stop.
"""
from __future__ import annotations

import asyncio
import ipaddress
import json
import logging
import os
import time
import urllib.request
from urllib.parse import urlparse

from .watch import AnchorWatch, WatchConfig

log = logging.getLogger("anchorwatch")
PATH = "navigation.position"
MODE_PATH = "automation.mode"         # published by the rules engine (D-052); ANCHOR = armed (D-092)
STW_PATH = "navigation.speedThroughWater"     # DST810 paddlewheel, m/s - context only
WIND_PATH = "environment.wind.speedApparent"  # gWind via GND 10, m/s - context only


def is_loopback(url: str) -> bool:
    host = urlparse(url).hostname or ""
    if host == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def check_target(url: str, allow_remote: bool) -> None:
    if not is_loopback(url) and not allow_remote:
        raise SystemExit(f"refusing non-loopback SignalK {url!r} (dry-run bench tool); "
                         "pass --allow-remote if you really mean it")


def stream_url(base: str) -> str:
    u = base.rstrip("/")
    u = "ws" + u[4:] if u.startswith("http") else u
    if "/signalk/v1/stream" not in u:
        u += "/signalk/v1/stream"
    return u + ("&" if "?" in u else "?") + "subscribe=none"


def subscribe_message(follow_mode: bool = False) -> dict:
    subs = [{"path": PATH, "period": 1000}, {"path": STW_PATH, "period": 5000}, {"path": WIND_PATH, "period": 5000}]
    if follow_mode:
        subs.append({"path": MODE_PATH, "policy": "instant"})
    return {"context": "vessels.self", "subscribe": subs}


def modes_from_delta(msg: dict) -> list[str]:
    """Every automation.mode value in a SignalK delta."""
    return [v["value"] for upd in msg.get("updates", []) or [] for v in upd.get("values", []) or []
            if v.get("path") == MODE_PATH and isinstance(v.get("value"), str)]


def numbers_from_delta(msg: dict, path: str) -> list[float]:
    """Every numeric value for one path in a SignalK delta."""
    return [float(v["value"]) for upd in msg.get("updates", []) or [] for v in upd.get("values", []) or []
            if v.get("path") == path and isinstance(v.get("value"), (int, float)) and not isinstance(v.get("value"), bool)]


def positions_from_delta(msg: dict) -> list[tuple[float, float]]:
    """(lat, lon) for every navigation.position value in a SignalK delta; anything else ignored."""
    out = []
    for upd in msg.get("updates", []) or []:
        for v in upd.get("values", []) or []:
            if v.get("path") != PATH or not isinstance(v.get("value"), dict):
                continue
            lat, lon = v["value"].get("latitude"), v["value"].get("longitude")
            if isinstance(lat, (int, float)) and isinstance(lon, (int, float)):
                out.append((float(lat), float(lon)))
    return out


def _headers() -> dict:
    tok = os.environ.get("SIGNALK_TOKEN")
    return {"Authorization": f"Bearer {tok}"} if tok else {}


class DryRun:
    """Glue: fixes in, log lines out. No outputs of any kind besides logging."""

    def __init__(self, cfg: WatchConfig, arm: bool, status_every: float = 60.0, clock=time.time,
                 rode_ft: float | None = None):
        self.watch, self.arm_on_first, self.status_every, self.clock = AnchorWatch(cfg), arm, status_every, clock
        self.rode_ft = rode_ft
        self.next_status = 0.0
        self.fixes = 0

    def _log_events(self, evs) -> None:
        for e in evs:
            if e.kind == "STATE":
                lvl = logging.CRITICAL if e.state == "ALARM" else logging.WARNING
                log.log(lvl, "%s -> %s: %s", e.prev, e.state, e.message)
            else:
                log.log(logging.WARNING if e.kind in ("WARN", "DRAG") else logging.INFO, "%s", e.message)

    def on_mode(self, mode: str, t: float | None = None) -> None:
        """--follow-mode: the boat mode is the arm switch (D-092). ANCHOR arms; anything else disarms."""
        t = self.clock() if t is None else t
        if mode == "ANCHOR" and self.watch.state == "OFF":
            log.info("boat mode ANCHOR - arming (dry run)")
            self._log_events(self.watch.arm(t, self.rode_ft))
        elif mode != "ANCHOR" and self.watch.state != "OFF":
            log.info("boat mode %s - disarming (dry run)", mode)
            self._log_events(self.watch.disarm(t))

    def on_fix(self, lat: float, lon: float, t: float | None = None) -> None:
        t = self.clock() if t is None else t
        self.fixes += 1
        if self.arm_on_first and self.watch.state == "OFF" and self.watch.armed_at is None:
            log.info("first fix %.6f,%.6f - arming (dry run)", lat, lon)
            self._log_events(self.watch.arm(t, self.rode_ft))
            self.arm_on_first = False
        self._log_events(self.watch.update(t, lat, lon))
        self.maybe_status(t, lat, lon)

    def on_context(self, stw_mps: float | None = None, wind_mps: float | None = None, t: float | None = None) -> None:
        self.watch.context(self.clock() if t is None else t, stw_mps=stw_mps, wind_mps=wind_mps)

    def on_tick(self, t: float | None = None) -> None:
        self._log_events(self.watch.tick(self.clock() if t is None else t))

    def maybe_status(self, t: float, lat: float, lon: float) -> None:
        if self.status_every <= 0 or t < self.next_status:
            return
        self.next_status = t + self.status_every
        s = self.watch.status(t)
        if s.state == "OFF":
            log.info("OFF (not armed) - position %.6f,%.6f, %d fixes seen", lat, lon, self.fixes)
            return
        d = f"{s.distance_m:.1f}" if s.distance_m is not None else "-"
        ar = f"{s.alarm_radius_m:.1f}" if s.alarm_radius_m is not None else "-"
        log.info("%s dist %s m alarm %s m%s", s.state, d, ar,
                 f" ({s.no_lock_reason})" if s.no_lock_reason else "")


async def run_ws(base: str, dry: DryRun, max_fixes: int | None = None, follow_mode: bool = False) -> None:
    import websockets
    url = stream_url(base)
    delay = 1.0
    while True:
        try:
            async with websockets.connect(url, additional_headers=_headers(), open_timeout=5) as ws:
                log.info("connected %s (read only)", url)
                await ws.send(json.dumps(subscribe_message(follow_mode)))
                delay = 1.0
                while True:
                    try:
                        raw = await asyncio.wait_for(ws.recv(), timeout=1.0)
                    except asyncio.TimeoutError:
                        dry.on_tick()
                        continue
                    try:
                        msg = json.loads(raw)
                    except ValueError:
                        continue
                    if follow_mode:
                        for m in modes_from_delta(msg):
                            dry.on_mode(m)
                    for v in numbers_from_delta(msg, STW_PATH):
                        dry.on_context(stw_mps=v)
                    for v in numbers_from_delta(msg, WIND_PATH):
                        dry.on_context(wind_mps=v)
                    for lat, lon in positions_from_delta(msg):
                        dry.on_fix(lat, lon)
                        if max_fixes is not None and dry.fixes >= max_fixes:
                            return
                    dry.on_tick()
        except Exception as exc:   # noqa: BLE001 - a watch keeps watching; log and reconnect
            log.warning("SignalK connection problem (%s: %s) - retry in %.0f s", type(exc).__name__, exc, delay)
            dry.on_tick()
            await asyncio.sleep(delay)
            delay = min(delay * 2, 30.0)


def run_poll(base: str, dry: DryRun, interval: float = 1.0, max_fixes: int | None = None) -> None:
    url = base.rstrip("/") + "/signalk/v1/api/vessels/self/navigation/position"
    last = None
    while max_fixes is None or dry.fixes < max_fixes:
        try:
            req = urllib.request.Request(url, headers=_headers())
            with urllib.request.urlopen(req, timeout=5) as r:
                body = json.loads(r.read().decode("utf-8"))
            val = body.get("value", body)
            key = (body.get("timestamp"), val.get("latitude"), val.get("longitude"))
            if key != last and isinstance(val.get("latitude"), (int, float)):
                last = key
                dry.on_fix(float(val["latitude"]), float(val["longitude"]))
        except (OSError, ValueError, AttributeError) as exc:
            log.warning("poll failed (%s)", exc)
        dry.on_tick()
        time.sleep(interval)


def main(args) -> int:
    check_target(args.signalk, args.allow_remote)
    follow = getattr(args, "follow_mode", False)
    if follow and (args.arm or args.poll):
        raise SystemExit("--follow-mode arms from the boat mode: drop --arm, and it needs the WebSocket (no --poll)")
    handlers = [logging.StreamHandler()]
    if args.log:
        handlers.append(logging.FileHandler(args.log, encoding="utf-8"))
    logging.basicConfig(level=logging.INFO, handlers=handlers,
                        format="%(asctime)s %(levelname)-8s %(message)s")
    cfg = WatchConfig(tolerance_m=args.tolerance, debounce_s=args.debounce)
    rode = getattr(args, "rode_ft", None)
    log.info("anchor watch DRY RUN on %s - reads only, drives nothing (tolerance %.0f m, debounce %.0f s, rode %s)",
             args.signalk, cfg.tolerance_m, cfg.debounce_s, f"{rode:.0f} ft" if rode else "not given")
    dry = DryRun(cfg, arm=args.arm, status_every=args.status_every, rode_ft=rode)
    try:
        if args.poll:
            run_poll(args.signalk, dry)
        else:
            asyncio.run(run_ws(args.signalk, dry, follow_mode=follow))
    except KeyboardInterrupt:
        log.info("stopped")
    return 0
