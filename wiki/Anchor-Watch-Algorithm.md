<!--
Purpose: The anchor-watch algorithm in plain terms - states, the circle fit and lock rule, the fallback,
  the coarse guard, the DRAG? warning, power cuts, conditions as context - and why each piece is there.
Version: v4 (2026-10-21) - v4: heading lock (ANC-40) = reference model only, PARKED; ANQ-03 / 04 parked. v3 (2026-10-07) - v3: parameters, transition table, fit / filter / DRAG? details, time base, restore
  details, vectors (rebuild-from-spec pass). v2 (2026-10-02) - v2: power cut (circle restored, no age limit - ANC-22), 15 m confirmed by the owner, RE-ARM,
  live in C (anchorwatch.c). v1: reference model only
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page. The executable truth is reference/anchorwatch/watch.py (+ its tests); the
  defaults table with the reasoning per number is in reference/README.md.
Invocation: n/a (reference doc)
-->

# Anchor-watch algorithm

## States

`OFF -> ACQUIRING -> LOCKED` (or `CONSERVATIVE` if no lock by 15 min, still trying to lock) `-> ALARM`
(latched until DISARM, a fresh ARM or RE-ARM - also across power cuts, ANC-22). The crew arms after the anchor is set (ANC-09), picking the rode
out in 50 ft steps (50 .. 300 ft, or ALL 328 ft - 100 m of chain). Live on the box as `main/anchorwatch.c`, which
matches this model in 20 simulated anchorages.

## Why a circle (ANC-02)

At anchor the boat swings on an arc AROUND the anchor. The average of the positions (the centroid method) sits
inside the arc, closer to the boat than the anchor really is. A circle fitted to the arc puts its centre on
the anchor. Heading is not used (the boat's heading data is unreliable, Ship's Automation F-100). The heading lock for a
narrow swing (ANC-40) is in the reference model only - PARKED (10-21, ANQ-34) until the CCU compass (~180 deg
out, F-226) is calibrated.

## Lock rule

Fixes are filtered (GPS jumps rejected, never more than 10 in a row), averaged into 10-s bins, and the circle
is refit over a sliding 15-min window. LOCK needs all of: >= 10 min observed, arc >= 40 deg, RMS <= 4 m,
arc depth >= 3 x RMS, centre moved < 3 m over the last 3 min, radius 15-250 m. On lock the centre and radius
FREEZE - a dragging anchor looks like a moving centre, so the watch never follows it.

## The circle (ANC-10)

LOCKED alarm radius = max(fitted radius, furthest 10-s average seen) + **15 m tolerance** (fixed - 10-02, to
tune after the shakedown, SPEC 5.14). Observed geometry only
- the rode does not set it. The boat is safe anywhere inside; only crossing the outer edge for 20 s alarms.

## CONSERVATIVE fallback (ANC-10)

No lock by 15 min (calm, little swing): centre = centroid of the fixes; radius = max(furthest seen,
2 x reach) + tolerance, reach = rode + 60 ft. Why 2 x: the anchor is within reach of every fix (so of their
centroid), and the boat may later lie anywhere within reach of the anchor. This removes the calm-then-180-degree
false alarm. The watch keeps trying to lock and switches to LOCKED when it can.

## Coarse guard (ANC-11)

While ACQUIRING there is no circle. The boat can never legitimately be more than 2 x reach from where it was
armed, so beyond 2 x reach + tolerance (20 s) is an ALARM. Catches a gross drag in the first minutes; the
crew watching the set covers the rest (the crew's workflow).

## DRAG? warning (ANC-12)

The owner: "a wind stretch stops, a drag keeps going." After LOCK / CONSERVATIVE, the distance from the frozen
centre is averaged in 10-s bins. Over the last 12 min, split into thirds: overall rate >= 0.015 m/s, rise >=
12 m, and EVERY third rising. A gust rises in one burst and stops (one third rises, two are flat) - no warning.
A drag rises evenly - warning, once, cleared when the whole window goes flat. Simulation: no warning in 48 h at
2 m GPS wander; 0.02 m/s drag warned 5-7 min and 0.03 m/s ~2 min before the alarm. It never alarms by itself.

## Power cut (ANC-22, 10-02)

Once a circle exists (LOCKED / CONSERVATIVE / ALARM) it is saved on every state change and comes back unchanged at
power-up - **no age limit**. The owner: "once our anchor is set the anchor watch shouldn't need to be reset - if we were to
recalc every 12 hrs that would allow a slow drift to go unrecognized." A drag during the outage alarms after the
usual 20 s; a latched ALARM stays latched; only DISARM / ARM / RE-ARM drops it (worst case a false alarm, never a
missed one). Armed with no circle yet -> re-armed fresh. `watch.py` snapshot() / restore(), `tests/test_restore.py`.

## RE-ARM (SPEC 5.13)

The skipper's practice on a night alarm: look at the breadcrumb trail, and if the risk is low (dragging away from land, no
boats behind) reset the circle. RE-ARM = a fresh ARM with the same rode, confirmed, his call. The only other time
the circle moves while armed is the one-time CONSERVATIVE -> LOCKED step.

## Conditions = context (ANC-12)

(In the reference model only - not yet ported to the box.) Water speed (DST810) and apparent wind (gWind) are quoted in warnings and alarms ("wind 28 kn, water speed
0.0 kn (moving with the water?)") and never change the circle. The physics: held in current = water speed
> 0; dragging with the water = ~0. Caveats: slack water also reads ~0; a wind drag moves through the water;
the DST810's direction sensing and low-speed cut-off are undocumented.

## Known limits

Tuned on simulation only (ANQ-03, ANQ-04 - both parked 10-21): real swings decide. GPS error is correlated in time, so the locked
centre can be several metres off; a drag then alarms later by about that error / drag speed. A fit made with
a slack rode is expected to go CONSERVATIVE rather than lock small - unproven. No re-lock after LOCK (RE-ARM to
refit). In a slip the boat barely moves (1 m seen 10-02): no lock, CONSERVATIVE at 15 min - as designed.

## Parameters (all in `aw_default_config`, anchorwatch.c = `WatchConfig`, watch.py)

| Parameter | Value | Role |
|---|---|---|
| `tolerance_m` | 15 | added to every alarm radius (10-02) |
| `debounce_s` | 20 | continuously outside this long -> ALARM |
| `max_speed_mps`, `jump_margin_m` | 3, 10 | a fix further than 3 x dt + 10 m from the last accepted fix is a jump |
| `reject_reset_count` | 10 | the 10th jump in a row is accepted (warning "N jumps in a row") |
| `bin_s` | 10 | averaging bin for the fit and for the trend |
| `fit_window_s` | 900 | sliding fit window |
| `min_fit_bins` | 12 | fewer bins -> "collecting (n/12 averaged positions)" |
| `lock_min_s` | 600 | no lock before 10 min after ARM |
| `min_arc_deg` | 40 | arc spanned by the bins around the fitted centre |
| `max_rms_m` | 4 | RMS of (distance to centre - r) over the bins |
| `min_sagitta_ratio` | 3 | sagitta r(1 - cos(arc/2)) >= 3 x RMS |
| `centre_stable_m`, `centre_stable_s` | 3, 180 | the fitted centre moved < 3 m over the last 180 s (needs 180 s of centre history) |
| `min_radius_m`, `max_radius_m` | 15, 250 | fitted radius outside -> no lock, centre history cleared |
| `no_lock_after_s` | 900 | still ACQUIRING at 15 min -> CONSERVATIVE |
| `stale_after_s` | 30 | no accepted fix this long -> GPS STALE warning |
| `bow_to_antenna_m` | 18.288 (60 ft) | reach = rode x 0.3048 + this |
| `trend_window_s`, `trend_segments` | 720, 3 | DRAG? window and its thirds |
| `trend_min_slope_mps`, `trend_min_rise_m` | 0.015, 12 | DRAG? thresholds |

Fixed buffers (anchorwatch.h): 1,200 raw fixes (ACQUIRING centroid), 128 fit bins, 64 centre-history points, 128
trend points. `aw_t` is ~25 KB and lives in PSRAM on the box.

## State machine

| From | Event | To | Side effects |
|---|---|---|---|
| any | ARM (rode or "don't know") | ACQUIRING | everything reset; arm time = now; first accepted fix = arm point + local frame origin; DO0 off; NVS `armed` + `rode` written, `snap` erased |
| any | DISARM | OFF | DO0 off; NVS `armed`, `rode`, `snap` erased |
| armed | RE-ARM (confirmed) | ACQUIRING | = ARM with the same rode (the last ARM's pick) |
| ACQUIRING | a fit passes every lock test | LOCKED | centre + radius frozen; alarm radius = max(fitted r, furthest 10-s bin in the fit window from the centre) + 15 m; fit window, raw fixes and trend cleared |
| ACQUIRING | 900 s since ARM, not locked | CONSERVATIVE | centre = centroid of all raw accepted fixes; alarm radius = max(furthest raw fix from it, 2 x reach if a rode was given) + 15 m |
| ACQUIRING | > 2 x reach + 15 m from the arm point for 20 s (only with a rode) | ALARM | coarse guard |
| CONSERVATIVE | a later fit passes every lock test | LOCKED | the one re-centring allowed while armed |
| LOCKED / CONSERVATIVE | > alarm radius from the centre for 20 s | ALARM | DO0 on, screen to WATCH at full brightness |
| ALARM | - | ALARM | latched: only DISARM / ARM / RE-ARM leave it; MUTE only stops the banner flashing |
| any armed | power cut with a circle (LOCKED / CONSERVATIVE / ALARM) | same state | restored at power-up from NVS `snap` |
| ACQUIRING | power cut (no circle) | ACQUIRING | re-armed fresh at power-up with the saved rode |

Warnings (not states): GPS STALE (once, after 30 s without an accepted fix; "GPS fixes resumed" on the next),
DRAG? (once, cleared by a flat window), "N jumps in a row - accepting the new position". DRAG? and STALE wake the
WATCH screen; only a STATE change into ALARM sets DO0.

## Exact rules

* **Local frame.** Equirectangular about the first fix: x = wrap(lon - lon0) x (pi/180) x R x cos(lat0),
  y = (lat - lat0) x (pi/180) x R, R = 6,371,008.8 m (mean Earth radius); 1 ft = 0.3048 m.
* **Filter.** Out-of-order or duplicate timestamps are rejected. A jump (above) is rejected unless it is the 10th
  in a row. A fix with exactly the same lat / lon as the last one is accepted (counts for the alarm) but not added
  to the fit.
* **Bins.** Bin index = floor((t - arm time) / 10 s); a bin closes when a fix lands in the next index; the closed
  bin's mean (t, x, y) enters the fit window and triggers a refit. Bins older than 900 s drop out.
* **Fit.** Kasa algebraic circle fit (least squares on x^2 + y^2 + Dx + Ey + F = 0, about the bins' mean), then up to
  5 Gauss-Newton iterations on the geometric residual (stop when the step is < 1e-4 m); if Gauss-Newton drives r <= 0
  the Kasa result is kept. Arc span = 360 deg minus the largest angular gap between bins around the centre.
* **CONSERVATIVE uses raw fixes, LOCKED uses bins** for "furthest seen".
* **Distance** is measured to the frozen centre (LOCKED / CONSERVATIVE / ALARM), to the provisional fit while
  ACQUIRING (display only), and to the arm point for the coarse guard.
* **DRAG?** Distance to the centre is binned in 10 s. Evaluated when the trend window holds >= 700 s of bins and at
  least 60 % of the expected 72 bins, each third >= 4 bins. Least-squares slope over the whole window `sl`; rise =
  sl x 720 s. Warn when sl >= 0.015 m/s AND rise >= 12 m AND the smallest third's slope >= 0.0075 m/s. Clear when
  sl <= 0.00375 m/s AND the newest third's slope <= 0.0075 m/s. Not evaluated in ALARM.
* **Coarse guard** needs a rode; "don't know" = no guard (the crew watching the set covers it).

## Time base on the box

The watch is clock-free: every timestamp is passed in. On the box `t` = seconds of box uptime (`esp_timer`), and a
fix's time = when n2k.c received the 129025 frame - NOT GPS time, so the watch works before GPS time is accepted and
cannot be confused by a time jump. `watch_ctl` runs at 1 Hz and feeds only the NEWEST position since the last one
fed (the reference model was designed for ~1 Hz; the MSC-10's actual 129025 rate is not recorded here), and never a fix received
before the ARM. `aw_tick` runs every second for the stale check.

## Power-cut restore - what comes back and what does not

Saved: state, centre (lat / lon), fitted radius, alarm radius, rode, locked flag (`aw_snapshot_t`, Data-and-Config).
At power-up `aw_restore` rebuilds the frame about the saved centre, sets arm time = now, and replays the state
change (event "RESTORED after power-up: ..."). Lost: fit window, centre history, trend bins and raw fixes. So after
a restore DRAG? needs a fresh 12 min of data, and a restored CONSERVATIVE keeps fitting and may still LOCK - but
not before 10 min after the restore (`lock_min_s` counts from the new arm time).

## Test vectors (C port = reference)

`tools/make_watch_vectors.py` runs `sim.py` anchorages through `watch.py` and writes `test_host/vectors/*.txt`
(generated, gitignored); `test_host/test_watch.c` replays them through `anchorwatch.c` and requires the same events
at the same seconds and the same final circle. Line format: `A t rode_ft` arm; `U t lat lon` fix; `K t` tick;
`D t` disarm; `R t_off t_on` power cut (snapshot at t_off, fresh watch restored at t_on); `E t kind state` expected
event (kind 0 STATE, 1 WARN, 2 DRAG, 3 INFO); `F state has_circle lat lon alarm_radius` final status. The 20
scenarios (rode 100 ft unless noted; fixed seeds): steady, large (60 m swing, 150 ft), small, none, reversal,
small-reversal, slow drag (0.03 m/s from 1 h), sudden drag, stretch (8 m wind stretch - must NOT warn), outliers
(2 %), 4 h of 3 m Gauss-Markov wander, coarse guard (0.5 m/s drag at 2 min), no rode, GPS gap (1500-1600 s),
duplicate / repeated fixes, RE-ARM at 2000 s + DISARM at 4000 s, and four power cuts: after LOCK, during a drag,
during ALARM, during CONSERVATIVE. Python-side cases: `reference/tests/` (test_watch, test_geo_fit,
test_rode_guard_drag, test_restore, test_replay, test_real_tracks - the 2021 Chesapeake 33.5 h track, no false alarm).
