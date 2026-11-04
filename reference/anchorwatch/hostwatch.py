"""
Purpose: Reference model of the box's "monitoring host down" watch (ANC-25, your-pi-hostname OQ-117 1b + 1c): decides
    from plain observations whether the Pi (SignalK over Wi-Fi) or the Cerbo (its N2K frames) is down, whether
    to assert DO1 (panel AL-13 'Automation'), and what the screen says. Pure logic - no I/O, no clock.
Version: v1 (2026-10-03)
Scope: STANDALONE UTILITY - the reference model; nothing on the box imports it (main/hostwatch.c is the C port,
    checked against this file by test_host/test_hostwatch.c via tools/make_hostwatch_vectors.py).
Architecture: reference/anchorwatch. Two independent watchers (PiWatch, CerboWatch) OR-ed into
    one output. The firmware's I/O task (main/hostwatch_ctl.c) does the polling / ping / N2K bookkeeping and
    feeds the results in; this file only decides.
Invocation: imported by tests/test_hostwatch.py and tools/make_hostwatch_vectors.py.

Rules (10-03, OQ-117):
  Pi     - unhealthy for down_after_s (60 s) -> ask for a router check; router answers -> ASSERT 'Pi down';
           router silent -> NO lamp, 'network down' (a router outage is not a Pi fault).
  Cerbo  - no frame from any of its N2K addresses for silent_s (180 s) while OTHER sources are still heard ->
           ASSERT 'Cerbo silent'; the whole bus silent (nothing from anyone for bus_quiet_s) -> NO lamp, 'N2K bus silent' (same attribution idea -
           DESIGN 10-03). Addresses not identified yet -> nothing to judge, no lamp.
  Both   - an asserted lamp is released ONLY after the device has been healthy for release_s (30 s) - never by
           the attribution changing (a lamp never goes out while its device is still down).
"""
from __future__ import annotations

from dataclasses import dataclass, field

# PiWatch / CerboWatch states (the C port uses the same numbers)
OK, UNKNOWN, DEGRADED, DOWN, NET_DOWN = 0, 1, 2, 3, 4
NAMES = {OK: "OK", UNKNOWN: "UNKNOWN", DEGRADED: "DEGRADED", DOWN: "DOWN", NET_DOWN: "NET_DOWN"}


@dataclass
class HostWatchConfig:
    pi_down_after_s: float = 60.0      # unhealthy this long -> router check -> assert
    cerbo_silent_s: float = 180.0      # no Cerbo frame this long -> assert (126993 every ~60 s)
    release_s: float = 30.0            # healthy this long -> release (no flapping)
    bus_quiet_s: float = 10.0          # no frame from ANY other source this long = the bus (or our CAN) is silent;
                                       # the MSC-10 alone sends position at 10 Hz, so 10 s is far outside normal


@dataclass
class PiWatch:
    cfg: HostWatchConfig = field(default_factory=HostWatchConfig)
    state: int = UNKNOWN
    asserted: bool = False
    bad_since: float | None = None
    good_since: float | None = None

    def wants_router(self, t: float) -> bool:
        """True when the next update() needs a router check (unhealthy long enough)."""
        return self.bad_since is not None and t - self.bad_since >= self.cfg.pi_down_after_s

    def update(self, t: float, pi_ok: bool, router_ok: bool | None = None) -> list[str]:
        ev: list[str] = []
        if pi_ok:
            self.bad_since = None
            if self.asserted:
                if self.good_since is None:
                    self.good_since = t
                if t - self.good_since >= self.cfg.release_s:
                    self.asserted = False
                    self.good_since = None
                    self.state = OK
                    ev.append("PI_RELEASED")
                else:
                    self.state = DOWN            # still lit, recovering
            else:
                if self.state == NET_DOWN:
                    ev.append("PI_NET_OK")
                self.state = OK
            return ev
        self.good_since = None
        if self.bad_since is None:
            self.bad_since = t
        if self.asserted:
            self.state = DOWN
            return ev
        if not self.wants_router(t):
            self.state = DEGRADED
            return ev
        if router_ok is None:                    # caller did not check: hold the current view
            if self.state not in (NET_DOWN,):
                self.state = DEGRADED
            return ev
        if router_ok:
            self.asserted = True
            self.state = DOWN
            ev.append("PI_DOWN")
        elif self.state != NET_DOWN:
            self.state = NET_DOWN
            ev.append("PI_NET_DOWN")
        return ev


@dataclass
class CerboWatch:
    cfg: HostWatchConfig = field(default_factory=HostWatchConfig)
    state: int = UNKNOWN
    asserted: bool = False
    known_since: float | None = None
    recovered_since: float | None = None

    def update(self, t: float, known: bool, last_cerbo_t: float | None, last_other_t: float | None) -> list[str]:
        """known = the Cerbo's addresses are identified; last_cerbo_t = newest frame from any of them;
        last_other_t = newest frame from any OTHER source (None = never)."""
        ev: list[str] = []
        if not known:
            self.known_since = None
            if not self.asserted:
                self.state = UNKNOWN
            return ev
        if self.known_since is None:
            self.known_since = t
        ref = self.known_since if last_cerbo_t is None else max(last_cerbo_t, self.known_since)
        silent = t - ref >= self.cfg.cerbo_silent_s
        if not silent:
            if self.asserted:
                if self.recovered_since is None:
                    self.recovered_since = max(last_cerbo_t if last_cerbo_t is not None else t, self.known_since)
                if t - self.recovered_since >= self.cfg.release_s:
                    self.asserted = False
                    self.recovered_since = None
                    self.state = OK
                    ev.append("CERBO_RELEASED")
                else:
                    self.state = DOWN
            else:
                if self.state == NET_DOWN:
                    ev.append("CERBO_BUS_OK")
                self.state = OK
            return ev
        self.recovered_since = None
        if self.asserted:
            self.state = DOWN
            return ev
        bus_alive = last_other_t is not None and t - last_other_t < self.cfg.bus_quiet_s
        if bus_alive:
            self.asserted = True
            self.state = DOWN
            ev.append("CERBO_DOWN")
        elif self.state != NET_DOWN:
            self.state = NET_DOWN
            ev.append("CERBO_BUS_SILENT")
        return ev


def screen_lines(pi: PiWatch, cerbo: CerboWatch) -> list[str]:
    """What the box shows (one line per problem; empty = all well). Same texts as main/hostwatch.c."""
    out = []
    if pi.state == DOWN:
        out.append("Pi / SignalK not answering")
    elif pi.state == NET_DOWN:
        out.append("Network down - router not answering")
    if cerbo.state == DOWN:
        out.append("Cerbo silent on N2K")
    elif cerbo.state == NET_DOWN:
        out.append("N2K bus silent")
    return out


def do1(pi: PiWatch, cerbo: CerboWatch) -> bool:
    return pi.asserted or cerbo.asserted
