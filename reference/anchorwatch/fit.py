"""
Purpose: Least-squares circle fit for the anchor watch - algebraic (Kasa) fit refined by a few
    Gauss-Newton steps on the geometric residual; returns centre, radius, RMS residual, the ARC SPAN
    (angular extent of the points around the centre, wrap-around safe) and the arc's sagitta.
Version: v1 (2026-09-30)
Scope: STANDALONE UTILITY - anchor-watch module (dry-run only, drives nothing).
Architecture: anchor-watch leaf module, pure stdlib (3x3 normal equations solved by hand). watch.py
    calls fit_circle() on 10-s averaged positions in local metres (geo.LocalFrame); centre = anchor,
    radius = rode + bow-to-antenna.
Invocation: imported by anchorwatch.watch; no CLI.
"""
from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Sequence

Point = tuple[float, float]


@dataclass(frozen=True)
class CircleFit:
    cx: float
    cy: float
    r: float
    rms: float            # RMS of (distance to centre - r), metres
    arc_span_deg: float   # angular extent of the points around (cx, cy)
    n: int

    @property
    def sagitta(self) -> float:
        """Depth of the observed arc (r - r*cos(span/2)). If this is not well above the noise the
        curvature is not really resolved and the centre is a guess."""
        return self.r * (1.0 - math.cos(math.radians(min(self.arc_span_deg, 360.0)) / 2.0))


def solve3(a: list[list[float]], b: list[float]) -> list[float] | None:
    """Solve a 3x3 linear system by Gaussian elimination with partial pivoting; None if singular."""
    m = [row[:] + [bv] for row, bv in zip(a, b)]
    for c in range(3):
        p = max(range(c, 3), key=lambda i: abs(m[i][c]))
        if abs(m[p][c]) < 1e-12:
            return None
        m[c], m[p] = m[p], m[c]
        for i in range(c + 1, 3):
            f = m[i][c] / m[c][c]
            for j in range(c, 4):
                m[i][j] -= f * m[c][j]
    x = [0.0, 0.0, 0.0]
    for i in (2, 1, 0):
        x[i] = (m[i][3] - sum(m[i][j] * x[j] for j in range(i + 1, 3))) / m[i][i]
    return x


def kasa(points: Sequence[Point]) -> tuple[float, float, float] | None:
    """Algebraic fit: minimise sum (x^2 + y^2 + D x + E y + F)^2. Data centred first for conditioning."""
    n = len(points)
    if n < 3:
        return None
    mx = sum(p[0] for p in points) / n
    my = sum(p[1] for p in points) / n
    ata = [[0.0] * 3 for _ in range(3)]
    atb = [0.0] * 3
    for px, py in points:
        x, y = px - mx, py - my
        row = (x, y, 1.0)
        rhs = -(x * x + y * y)
        for i in range(3):
            atb[i] += row[i] * rhs
            for j in range(3):
                ata[i][j] += row[i] * row[j]
    sol = solve3(ata, atb)
    if sol is None:
        return None
    d, e, f = sol
    cx, cy = -d / 2.0, -e / 2.0
    r2 = cx * cx + cy * cy - f
    if r2 <= 0:
        return None
    return cx + mx, cy + my, math.sqrt(r2)


def refine(points: Sequence[Point], cx: float, cy: float, r: float, iters: int = 5):
    """Gauss-Newton on the geometric residual d_i - r. Returns the start values if a step fails."""
    for _ in range(iters):
        jtj = [[0.0] * 3 for _ in range(3)]
        jtr = [0.0] * 3
        for px, py in points:
            dx, dy = px - cx, py - cy
            d = math.hypot(dx, dy)
            if d < 1e-9:
                continue
            res = d - r
            jrow = (-dx / d, -dy / d, -1.0)
            for i in range(3):
                jtr[i] += jrow[i] * res
                for j in range(3):
                    jtj[i][j] += jrow[i] * jrow[j]
        step = solve3(jtj, [-v for v in jtr])
        if step is None:
            break
        cx, cy, r = cx + step[0], cy + step[1], r + step[2]
        if r <= 0:
            return None
        if math.hypot(step[0], step[1]) < 1e-4 and abs(step[2]) < 1e-4:
            break
    return cx, cy, r


def arc_span_deg(points: Sequence[Point], cx: float, cy: float) -> float:
    """360 minus the largest angular gap between consecutive point bearings around (cx, cy)."""
    if len(points) < 2:
        return 0.0
    angs = sorted(math.degrees(math.atan2(py - cy, px - cx)) % 360.0 for px, py in points)
    gaps = [b - a for a, b in zip(angs, angs[1:])]
    gaps.append(angs[0] + 360.0 - angs[-1])   # wrap-around gap
    return 360.0 - max(gaps)


def fit_circle(points: Sequence[Point], do_refine: bool = True) -> CircleFit | None:
    """Fit a circle to >= 3 points (metres). None if degenerate (collinear / too few points)."""
    k = kasa(points)
    if k is None:
        return None
    if do_refine:
        k = refine(points, *k) or k
    cx, cy, r = k
    rms = math.sqrt(sum((math.hypot(x - cx, y - cy) - r) ** 2 for x, y in points) / len(points))
    return CircleFit(cx, cy, r, rms, arc_span_deg(points, cx, cy), len(points))
