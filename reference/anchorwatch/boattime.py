"""
Purpose: Boat time for the anchor box CLOCK (SPEC 3, ANC-07 / ANC-08): local zone from position (built-in table,
    US daylight-saving rule, nautical-zone fallback) with a 5-minute steadiness rule and a manual pin; GPS time
    acceptance (two consistent N2K 126992 messages before the clock shows anything); sun altitude -> night.
Version: v1 (2026-10-02)
Scope: STANDALONE UTILITY - reference model; the ESP32 C port (main/boattime.c) must pass the same cases.
Architecture: reference, pure stdlib, clock-free (the caller passes every timestamp).
    tests/test_boattime.py checks it against the IANA tz database (tzdata) and astral; tools/make_c_vectors.py
    exports the cases for the C host test. Zone boundaries: wiki/Time-and-Zones.md.
Invocation: imported by tests and tools/make_c_vectors.py; no CLI.
"""
from __future__ import annotations

import math
from dataclasses import dataclass

# --------------------------------------------------------------------------- zones

US_DST = "US"      # 2nd Sunday March 02:00 local standard -> 1st Sunday November 02:00 local daylight
NO_DST = ""


@dataclass(frozen=True)
class Zone:
    zid: str          # short id, shown on WATCH
    name: str
    std_min: int      # standard offset from UTC, minutes (east positive)
    dst: str          # US_DST or NO_DST


@dataclass(frozen=True)
class Area:
    zone: Zone
    lat_min: float
    lat_max: float
    lon_min: float
    lon_max: float

    def contains(self, lat: float, lon: float) -> bool:
        return self.lat_min <= lat < self.lat_max and self.lon_min <= lon < self.lon_max


US_CENTRAL = Zone("CT", "US Central", -360, US_DST)
US_EASTERN = Zone("ET", "US Eastern", -300, US_DST)
BAHAMAS = Zone("BS", "Bahamas / Turks & Caicos (Eastern)", -300, US_DST)
ATLANTIC = Zone("AT", "Atlantic (PR, VI, DR, Lesser Antilles, ABC)", -240, NO_DST)

# First match wins. Florida's coastal Central / Eastern line = the Bay / Gulf county line between Mexico Beach
# (Central) and Port St. Joe (Eastern), ~85.39 W. The boxes include nearby open water on purpose (DESIGN 10-02):
# a Gulf crossing keeps US time rather than flipping to the nautical zone.
AREAS: tuple[Area, ...] = (
    Area(US_EASTERN, 24.0, 31.0, -85.39, -79.6),     # Florida from Port St. Joe east, the Keys, FL east coast
    Area(US_CENTRAL, 25.0, 31.5, -97.6, -85.39),     # northern Gulf: Texas .. NOLA .. Pensacola .. Mexico Beach
    Area(BAHAMAS, 20.9, 27.5, -80.6, -71.0),         # Bahamas + Turks & Caicos (Cuba's north coast falls in: its
                                                     # DST changes on the same days but at other hours)
    Area(ATLANTIC, 13.0, 20.5, -71.6, -59.0),        # DR (east of Haiti), PR, VI, ownerwards, Windwards
    Area(ATLANTIC, 10.0, 13.0, -71.0, -59.0),        # ABC islands, Grenada, Trinidad (Colombia's Guajira excluded)
)


def nautical_zone(lon: float) -> Zone:
    """Offshore / outside the table: whole hours = longitude / 15, rounded (boundaries at 7.5 deg)."""
    h = int(math.floor(lon / 15.0 + 0.5))
    h = max(-12, min(12, h))
    zid = "Z" if h == 0 else f"N{h:+d}"
    return Zone(zid, f"nautical {h:+d}", h * 60, NO_DST)


def zone_at(lat: float, lon: float) -> Zone:
    for a in AREAS:
        if a.contains(lat, lon):
            return a.zone
    return nautical_zone(lon)


ZONES_BY_ID = {z.zid: z for z in (US_CENTRAL, US_EASTERN, BAHAMAS, ATLANTIC)}


def zone_by_id(zid: str) -> Zone:
    """Pick-list ids: the table zones, or nautical 'N-5', 'Z', 'N+3' ..."""
    if zid in ZONES_BY_ID:
        return ZONES_BY_ID[zid]
    if zid == "Z":
        return nautical_zone(0.0)
    if zid.startswith("N"):
        return nautical_zone(int(zid[1:]) * 15.0)
    raise KeyError(zid)


# --------------------------------------------------------------------------- calendar (UTC, no tz library)

def days_from_civil(y: int, m: int, d: int) -> int:
    """Days since 1970-01-01 (H. Hinnant's algorithm, public domain)."""
    y -= m <= 2
    era = (y if y >= 0 else y - 399) // 400
    yoe = y - era * 400
    doy = (153 * (m + (-3 if m > 2 else 9)) + 2) // 5 + d - 1
    doe = yoe * 365 + yoe // 4 - yoe // 100 + doy
    return era * 146097 + doe - 719468


def civil_from_days(z: int) -> tuple[int, int, int]:
    z += 719468
    era = (z if z >= 0 else z - 146096) // 146097
    doe = z - era * 146097
    yoe = (doe - doe // 1460 + doe // 36524 - doe // 146096) // 365
    y = yoe + era * 400
    doy = doe - (365 * yoe + yoe // 4 - yoe // 100)
    mp = (5 * doy + 2) // 153
    d = doy - (153 * mp + 2) // 5 + 1
    m = mp + 3 if mp < 10 else mp - 9
    return y + (m <= 2), m, d


def _nth_sunday(y: int, m: int, n: int) -> int:
    """Day number (since 1970) of the n-th Sunday of a month."""
    first = days_from_civil(y, m, 1)
    wd = (first + 4) % 7          # 1970-01-01 was a Thursday; 0 = Sunday
    return first + (7 - wd) % 7 + 7 * (n - 1)


def us_dst_active(utc_s: int, std_min: int) -> bool:
    y, _, _ = civil_from_days(utc_s // 86400)
    start = _nth_sunday(y, 3, 2) * 86400 + 2 * 3600 - std_min * 60          # 02:00 standard
    end = _nth_sunday(y, 11, 1) * 86400 + 2 * 3600 - (std_min + 60) * 60    # 02:00 daylight
    return start <= utc_s < end


def offset_min(zone: Zone, utc_s: int) -> int:
    if zone.dst == US_DST and us_dst_active(utc_s, zone.std_min):
        return zone.std_min + 60
    return zone.std_min


# --------------------------------------------------------------------------- zone choice (auto + pin)

ZONE_STEADY_S = 300   # a new zone from position must hold this long before the clock follows it (DESIGN)


class ZoneChooser:
    """AUTO: follow zone_at(position), but only after the new zone has held for ZONE_STEADY_S (no flicker when
    anchored on a line). PINNED: a fixed zone until unpinned (SPEC 3.5); the clock shows 'M'."""

    def __init__(self) -> None:
        self.current: Zone | None = None
        self._cand: Zone | None = None
        self._cand_since = 0
        self.pinned: Zone | None = None

    def pin(self, zone: Zone | None) -> None:
        self.pinned = zone

    def update(self, t_s: int, lat: float | None, lon: float | None) -> Zone | None:
        if lat is not None and lon is not None:
            z = zone_at(lat, lon)
            if self.current is None:
                self.current = z
            elif z.zid == self.current.zid:
                self._cand = None
            elif self._cand is None or self._cand.zid != z.zid:
                self._cand, self._cand_since = z, t_s
            elif t_s - self._cand_since >= ZONE_STEADY_S:
                self.current, self._cand = z, None
        return self.zone()

    def zone(self) -> Zone | None:
        return self.pinned if self.pinned is not None else self.current

    @property
    def manual(self) -> bool:
        return self.pinned is not None


# --------------------------------------------------------------------------- GPS time acceptance

MIN_VALID_UTC_S = days_from_civil(2026, 1, 1) * 86400   # sanity floor (bad receivers / week rollover)
AGREE_MS = 2000


class TimeSync:
    """UTC from N2K 126992 (source kind 0 = GPS only). The clock is set only when two consecutive messages agree
    with each other (within AGREE_MS of the elapsed local time); a later jump also needs two agreeing messages.
    `mono_ms` = the box's own monotonic clock."""

    def __init__(self) -> None:
        self._utc_ms: int | None = None      # accepted UTC at _mono_ms
        self._mono_ms = 0
        self._last: tuple[int, int] | None = None   # previous message (utc_ms, mono_ms)

    def feed(self, utc_ms: int, mono_ms: int, kind: int = 0) -> None:
        if kind != 0 or utc_ms < MIN_VALID_UTC_S * 1000:
            return
        prev, self._last = self._last, (utc_ms, mono_ms)
        if self._utc_ms is not None and abs(self.now_ms(mono_ms) - utc_ms) <= AGREE_MS:
            self._utc_ms, self._mono_ms = utc_ms, mono_ms      # in step: follow GPS
            return
        if prev is not None and abs((prev[0] + (mono_ms - prev[1])) - utc_ms) <= AGREE_MS:
            self._utc_ms, self._mono_ms = utc_ms, mono_ms      # two agreeing messages: (re)set

    def valid(self) -> bool:
        return self._utc_ms is not None

    def now_ms(self, mono_ms: int) -> int:
        assert self._utc_ms is not None
        return self._utc_ms + (mono_ms - self._mono_ms)


# --------------------------------------------------------------------------- sun -> night

NIGHT_ALT_DEG = -0.833   # sunset / sunrise: upper limb on the horizon with standard refraction


def sun_altitude_deg(utc_s: int, lat: float, lon: float) -> float:
    """Geometric sun altitude, NOAA Solar Calculator method (Meeus-based, public domain; NOAA GML
    'solar calculation details' spreadsheet) - good to ~0.01 deg for these years."""
    jd = utc_s / 86400.0 + 2440587.5
    t = (jd - 2451545.0) / 36525.0
    l0 = (280.46646 + t * (36000.76983 + t * 0.0003032)) % 360.0
    m = math.radians(357.52911 + t * (35999.05029 - 0.0001537 * t))
    e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t)
    c = (math.sin(m) * (1.914602 - t * (0.004817 + 0.000014 * t)) + math.sin(2 * m) * (0.019993 - 0.000101 * t)
         + math.sin(3 * m) * 0.000289)
    om = math.radians(125.04 - 1934.136 * t)
    app = math.radians(l0 + c - 0.00569 - 0.00478 * math.sin(om))
    eps0 = 23.0 + (26.0 + (21.448 - t * (46.815 + t * (0.00059 - t * 0.001813))) / 60.0) / 60.0
    eps = math.radians(eps0 + 0.00256 * math.cos(om))
    decl = math.asin(math.sin(eps) * math.sin(app))
    y = math.tan(eps / 2.0) ** 2
    l0r = math.radians(l0)
    eqt = 4.0 * math.degrees(y * math.sin(2 * l0r) - 2 * e * math.sin(m) + 4 * e * y * math.sin(m) * math.cos(2 * l0r)
                             - 0.5 * y * y * math.sin(4 * l0r) - 1.25 * e * e * math.sin(2 * m))
    tst = (utc_s % 86400) / 60.0 + eqt + 4.0 * lon
    ha = math.radians(tst / 4.0 - 180.0)
    la = math.radians(lat)
    cz = math.sin(la) * math.sin(decl) + math.cos(la) * math.cos(decl) * math.cos(ha)
    return 90.0 - math.degrees(math.acos(max(-1.0, min(1.0, cz))))


def is_night(utc_s: int, lat: float, lon: float) -> bool:
    return sun_altitude_deg(utc_s, lat, lon) < NIGHT_ALT_DEG


# --------------------------------------------------------------------------- what the CLOCK shows

@dataclass(frozen=True)
class ClockFace:
    hh: int          # -1 -> "--:--"
    mm: int
    manual: bool     # 'M'
    night: bool


def clock_face(sync: TimeSync, chooser: ZoneChooser, mono_ms: int,
               lat: float | None, lon: float | None) -> ClockFace:
    """Never a wrong number: '--:--' until GPS time is accepted AND a zone is known (position or pin)."""
    z = chooser.zone()
    if not sync.valid() or z is None:
        return ClockFace(-1, 0, chooser.manual, False)
    utc_s = sync.now_ms(mono_ms) // 1000
    local = utc_s + offset_min(z, utc_s) * 60
    sod = local % 86400
    night = lat is not None and lon is not None and is_night(utc_s, lat, lon)
    return ClockFace(sod // 3600, sod // 60 % 60, chooser.manual, night)
