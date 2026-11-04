<!--
Purpose: What the anchor watch is, its algorithm and lock rule, defaults and why, how to test / replay /
  dry-run it, and its known limits.
Version: v4 (2026-10-03) - v4: hostwatch.py (monitoring-host watch, DO1 -> AL-13, ANC-25), 163 tests. v3: boattime.py (boat time), watch.py snapshot() / restore() (power cut, no age limit), 148 tests; the C ports on the box are checked against this model (test_host/). v2: rode pick, rode-floored conservative circle, coarse guard, DRAG? warning,
  water speed + wind as context (10-02; ./SPEC.md s.5). v1 (2026-09-30)
Scope: STANDALONE UTILITY - the reference model + tests; nothing on the box imports it (the C ports in main/ are checked against it).
Architecture: reference - the Python REFERENCE MODEL of the anchor box (moved out of your-pi-hostname stacks/anchor-watch 2026-10-02, ANC-14); the ESP32 C port must pass the same cases. geo.py + fit.py -> watch.py (state machine); sim.py,
  replay.py, service.py and __main__.py around it; tests/ (pytest, offline).
Invocation: n/a (reference doc)
-->

# anchor-watch

A circle-fit anchor alarm, built from the original design (OQ-098). **Dry run only**: it reads
positions and logs what it *would* do. It drives no relay, horn or SignalK path.

## Why

The boat's current anchor alarm takes the **centroid** of the early fixes as the anchor. That is the
wrong point. At anchor the boat swings on an **arc around** the anchor, so the centroid sits inside the
arc, closer to the boat than the anchor really is. This watch fits a **circle** to the swing instead:
the centre is the anchor, and the radius is rode plus bow-to-antenna. Heading and motion sensors are
not used, because the boat's heading data is unreliable today (F-100).

## Layout

| File | What |
|---|---|
| `anchorwatch/geo.py` | Local tangent plane (equirectangular, metres about a reference fix) and back, haversine, offset |
| `anchorwatch/fit.py` | Kasa algebraic circle fit, then Gauss-Newton refinement. Returns centre, radius, RMS, arc span (wrap-safe) and sagitta |
| `anchorwatch/watch.py` | The state machine (`AnchorWatch`, `WatchConfig`). Clock-free: the caller passes every timestamp. `snapshot()` / `restore()` = the circle survives a power cut, no age limit (10-02) |
| `anchorwatch/hostwatch.py` | The monitoring-host watch (ANC-25): `PiWatch` (unhealthy 60 s + router answers -> DO1; router silent = network down, no lamp) and `CerboWatch` (silent 180 s while the bus lives -> DO1); 30 s healthy releases. Spec for main/hostwatch.c |
| `anchorwatch/boattime.py` | Boat time for the CLOCK: zone table + US DST + nautical, 5-min steadiness, pin, GPS time acceptance, NOAA sun -> night |
| `anchorwatch/sim.py` | Synthetic swings (steady, large, small, none, reversal, small-reversal), GPS noise, outliers, slow or sudden drag, GPX writer |
| `anchorwatch/replay.py` | Replays a GPX or CSV track and prints a timeline |
| `anchorwatch/service.py` | Dry-run live watch on a loopback SignalK (WebSocket, or REST with `--poll`) |
| `tests/` | 148 pytest tests, fully offline (tzdata + astral test-only). `test_rode_guard_drag.py`, `test_restore.py`, `test_boattime.py` = the 10-02 additions. The C ports in `main/` are checked against this model by `../test_host/run.bat` |

## Algorithm

1. **ARM** puts the watch in `ACQUIRING`. Each fix is projected to metres about the first fix.
2. **Filtering.** A fix further than `max_speed * dt + jump_margin` from the last accepted fix is a GPS
   jump and is rejected. So is a fix that is out of order or has a duplicate timestamp. Both kinds are
   counted. After 10 jumps in a row the new position is accepted anyway, so the filter can never hide
   a real move. A fix that repeats the same coordinates still counts for the alarm but is not added to
   the fit.
3. **Averaging.** Fixes are averaged into 10-s bins, which cuts white GPS noise by about 3x. The circle is refit
   each time a bin closes, over a sliding 15-min window of bins.
4. **LOCK** needs ALL of the following:
   * at least 10 min since ARM;
   * an arc span of at least 40°;
   * an RMS residual of at most 4 m;
   * an arc depth (sagitta) of at least 3x the RMS, which proves the curvature is really resolved;
   * a fitted centre that has moved less than 3 m over the last 3 min;
   * a radius between 15 and 250 m.

   On lock the centre and radius are **FROZEN**. A dragging anchor looks like a moving centre, so the
   watch never follows it. The alarm circle is `max(fitted radius, furthest 10-s average seen) +
   tolerance`. The circle therefore never comes out smaller than the swing it was fitted to.
5. **CONSERVATIVE.** If there is no lock by 15 min, the watch freezes a circle made from the centroid of
   all accepted fixes, with radius = the maximum distance seen from it + tolerance. The status and log
   say "no lock - conservative". The watch keeps trying to lock and switches to `LOCKED` if it later
   succeeds.
6. **ALARM** fires when the boat is continuously outside the alarm circle for 20 s or more. One stray fix
   cannot trigger it. `ALARM` is latched until `disarm()` or a fresh `arm()`.
7. **Rode pick (10-02).** ARM takes the rode out, picked in 50 ft steps (no chain counter aboard - it is a
   guess). reach = rode + bow-to-antenna (the MSC-10 is ~60 ft aft of the bow roller). The rode NEVER sets a
   LOCKED circle - that is observed geometry + tolerance, because the rode ignores depth and roller height.
8. **Rode floor on CONSERVATIVE.** With no lock there is no observed geometry, so the conservative circle is
   at least 2 x reach + tolerance: the anchor is within reach of every fix seen (so of their centroid), and the
   boat can later lie anywhere within reach of the anchor. This removes the calm-then-180 false alarm.
9. **Coarse guard while ACQUIRING.** ALARM if the boat is more than 2 x reach + tolerance from where the watch
   was armed, for the 20 s debounce. A normal swing can never get that far. No rode -> no guard.
10. **DRAG? warning** (owner: "a wind stretch stops, a drag keeps going"). After LOCK / CONSERVATIVE the distance
   from the frozen centre is averaged in 10-s bins. Over the last 12 min, split in thirds: whole-window rate
   >= 0.015 m/s, rise >= 12 m, and EVERY third rising (>= half that rate) -> one DRAG warning. A stretch rises
   in one burst then stops, so two thirds are flat. Clears when the whole window is flat. Never alarms by itself.
11. **Conditions = context only.** `context(t, stw_mps, wind_mps)` (DST810 water speed, gWind apparent wind)
   are quoted in DRAG? and WOULD ALARM lines when fresh (< 60 s) and never change the circle. A water speed
   under 0.1 kn is flagged "moving with the water?" (owner: held in current = water speed > 0).
12. **GPS stale.** `tick(t)` warns once when there has been no accepted fix for 30 s, and reports again
   when fixes resume.

## Defaults (`WatchConfig`, all overridable)

| Parameter | Default | Why |
|---|---|---|
| `tolerance_m` | 15 | From the spec. Sits above GPS wander plus fit error |
| `debounce_s` | 20 | From the spec. Rides out single jumps and short multipath |
| `max_speed_mps` + `jump_margin_m` | 3 + 10 m | A boat at anchor never moves 3 m/s. The margin is needed because at 1 Hz two 2.5 m-sigma fixes differ by more than 3 m about half the time |
| `reject_reset_count` | 10 | After 10 s of a consistently "impossible" position, believe it |
| `bin_s` | 10 | Averages white noise. The boat moves only ~1 m along the arc in 10 s |
| `fit_window_s` | 900 | A sliding window, so an anchor that is still setting or dragging keeps the centre moving and blocks the lock |
| `lock_min_s` / `no_lock_after_s` | 600 / 900 | the 10-15 min default |
| `min_arc_deg` | 40 | From the spec |
| `max_rms_m` | 4 | From the spec. RMS is measured on 10-s averages, so real values are ~1 m in simulation |
| `min_sagitta_ratio` | 3 | Added. Without it a near-straight or blob-shaped track can fake a confident small circle |
| `centre_stable_m` / `_s` | 3 m / 180 s | From the spec |
| `min_radius_m` / `max_radius_m` | **15** / 250 | The spec said 5. See the note below |
| `stale_after_s` | 30 | Warn when the watch has gone blind |
| `bow_to_antenna_m` | 18.3 (60 ft) | 10-02: the MSC-10 is at the back, ~60 ft from the bow |
| `trend_window_s` / `trend_segments` | 720 / 3 | Sim 10-02: no DRAG? in 48 h at 2 m GPS wander (2 at 3 m); a 0.02 m/s drag warns 5-7 min and 0.03 m/s ~2 min before the ALARM |
| `trend_min_slope_mps` / `trend_min_rise_m` | 0.015 / 12 | Faster drags hit the circle first - the warning is for slow ones |
| `context_fresh_s` | 60 | Older water speed / wind is not quoted |

**Why `min_radius_m` is 15 and not 5.** Rode plus bow-to-antenna on a 51' boat is never under ~15 m.
In simulation with 3-5 m of slow GPS wander, a wander blob fitted a 5-8 m "circle" that passed every
other test. With a 15 m floor that false lock is gone. A true radius under 15 m would simply never
lock, and the watch would go CONSERVATIVE instead, which is a safe failure.

## Run

Everything runs from `reference`, with its own `.venv` (Python 3.14; pytest and
websockets are already in it):

```
.venv\Scripts\python -m pytest                       # 148 tests, ~10 s, offline
.venv\Scripts\python -m anchorwatch sim --out swing.gpx --pattern steady --hours 2 --drag slow --drag-at 3600 --every 2
.venv\Scripts\python -m anchorwatch replay swing.gpx [--arm-at 300 | --arm-at 2026-11-05T22:10:00Z] [--rode-ft 150] [--tolerance 15] [--every 600]
.venv\Scripts\python -m anchorwatch run --signalk http://127.0.0.1:3100 --arm [--rode-ft 150] [--tolerance 15] [--poll] [--log aw.log]
```

* `replay` accepts GPX (`trkpt`, any namespace, 1-5 s spacing is fine) or CSV with a `t,lat,lon` header,
  where `t` is epoch seconds or ISO-8601. It arms at the first fix unless you give `--arm-at`.
* `run` subscribes to `navigation.position`, plus `navigation.speedThroughWater` and
  `environment.wind.speedApparent` as context. It refuses any non-loopback host unless you pass
  `--allow-remote`, so it cannot be pointed at the boat by accident. If SignalK security is on, a
  read token comes from `$SIGNALK_TOKEN`. Fix times are local receive times.

## Known limits

* **Tuning needs real swing data.** a handheld-GPS walk (circles, arcs and simulated drags; the
  GPX file has not been located yet) and the shakedown anchor recordings (early Nov 2026, logged from
  `navigation.position` on the bench history) come first. Every default above is a first guess checked
  only against `sim.py`.
* **GPS error is correlated in time, not white.** With `--walk` (Gauss-Markov wander) at 3 m, the
  locked centre is typically 3-10 m off in simulation; at 5 m it is up to ~14 m off. There are no false
  alarms, but a drag alarms later by roughly the centre error divided by the drag speed.
* **Without a rode pick, a conservative circle can false-alarm on a later wind shift** (with a rode the floor fixes it). If the first 15 min show almost no
  swing, the conservative circle is small. A later 180° reversal carries the boat outside it before the
  new arc can lock. This is pinned by a test and is an announced, loud, safe failure. The remedies need
  owner input (see the open questions).
* **Drag during ACQUIRING.** Only the coarse guard (2 x reach + tolerance from the arming point) watches the
  first 10-15 min - it catches a gross drag, not a slow one. the crew's workflow covers this window: the crew
  watches, the back-down shows the hold, pick up and restart if not, free-dive check when possible.
* **DRAG? on GPS wander.** The trend thresholds are tuned on simulated wander only; real MSC-10 data decides.
* **Slack-rode lock.** A fit made in a calm with the rode slack is expected to fail the lock tests and go
  CONSERVATIVE rather than lock small - unproven until real swings are replayed.
* **Detection delay** after the boat truly crosses R + tolerance, in simulation: sudden drag ~20-30 s,
  0.1 m/s ~60-90 s, 0.05 m/s ~60-110 s, 0.03 m/s ~190-220 s. The alarm circle is floored at the furthest
  swing seen, and the 20 s debounce adds to that.
* There is no heading or motion input, no current or tide model, and no re-lock after LOCK. Re-arm to
  refit.
