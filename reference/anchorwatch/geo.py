"""
Purpose: Geometry helpers for the anchor watch - a local tangent plane (equirectangular about a
    reference fix, metres east/north) and back, plus great-circle distance and a lat/lon offset.
Version: v1 (2026-09-30)
Scope: STANDALONE UTILITY - anchor-watch module (dry-run only, drives nothing).
Architecture: anchor-watch leaf module, pure stdlib. Used by fit.py callers (watch.py projects every
    fix into one LocalFrame so the circle fit works in metres), sim.py and replay.py. At anchor-watch
    scales (< 1 km) the equirectangular error is millimetres.
Invocation: imported by anchorwatch.watch / sim / replay; no CLI.
"""
from __future__ import annotations

import math
from dataclasses import dataclass

R_EARTH_M = 6_371_008.8   # mean Earth radius (IUGG)


def _wrap_deg(d: float) -> float:
    """Wrap a longitude difference into [-180, 180)."""
    return (d + 180.0) % 360.0 - 180.0


@dataclass(frozen=True)
class LocalFrame:
    """Flat x (east) / y (north) metres about (lat0, lon0)."""
    lat0: float
    lon0: float

    @property
    def _kx(self) -> float:
        return math.radians(1.0) * R_EARTH_M * math.cos(math.radians(self.lat0))

    def to_xy(self, lat: float, lon: float) -> tuple[float, float]:
        k = math.radians(1.0) * R_EARTH_M
        return _wrap_deg(lon - self.lon0) * self._kx, (lat - self.lat0) * k

    def to_latlon(self, x: float, y: float) -> tuple[float, float]:
        k = math.radians(1.0) * R_EARTH_M
        lon = _wrap_deg(self.lon0 + x / self._kx)
        return self.lat0 + y / k, lon


def distance_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Haversine great-circle distance in metres."""
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(_wrap_deg(lon2 - lon1))
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * R_EARTH_M * math.asin(min(1.0, math.sqrt(a)))


def offset(lat: float, lon: float, bearing_deg: float, dist_m: float) -> tuple[float, float]:
    """The point dist_m from (lat, lon) on a true bearing (flat approximation, fine under ~1 km)."""
    b = math.radians(bearing_deg)
    return LocalFrame(lat, lon).to_latlon(dist_m * math.sin(b), dist_m * math.cos(b))
