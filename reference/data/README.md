<!--
Purpose: Index of REAL recorded GPS tracks used to test the anchor watch (ANQ-03).
Version: v2 (2026-10-08) - v2: synth/ - the 10-08 synthetic drift scenario. v1 (2026-10-02)
Scope: STANDALONE UTILITY - test data for the anchor-box reference model.
Architecture: reference/data; read by tests/test_real_tracks.py and by
  `python -m anchorwatch replay <file>`. Originals stay where were downloaded them; copies here are the fixtures.
Invocation: n/a (reference doc)
-->

# Real tracks

| File | From | What | Use |
|---|---|---|---|
| `explore/2021-10-08_chesapeake_anchorage.gpx` | Garmin Explore, Downloads\Track Me.gpx | 1,006 fixes, 2021-10-08 01:19Z .. 10-09 10:50Z (33.5 h), ~2 min spacing (max 10 min), at 37.6598 N 76.4429 W | Real night at anchor: never alarms; locks r ~21 m once bins are scaled to the log rate (test_real_tracks.py) |
| `explore/2025-04-12_walk_untimed.gpx` | Garmin Explore, Downloads\2025-04-12 162008.gpx | 80 points, **no timestamps**, elevation ~-157 m (likely an uncalibrated barometric altimeter - ground there is ~20 m; confirmed 10-02; position unaffected), ~70 m patch at 28.1324 N 81.4616 W | Not replayable (the replay drops untimed points). Explore has thinned it; the timed original is on the Montana |

The box sees ~1 Hz GPS; Explore logs are sparse. Default config (10 s bins, 12 in 15 min) cannot lock on a
2-minute log - that is the log, not the model. The full-rate originals are on the Montana (`Garmin\GPX\`, USB-C
data cable) - put them in `montana/`.

# Synthetic tracks (`synth/`)

| File | Made by | What | Replay result (default config, `--rode-ft 150`, 10-08) |
|---|---|---|---|
| `synth/drift_scenario_1008.gpx` | `synth/make_drift_scenario_1008.py --seed 1` | 10-08: set on a ~150 ft swing, 20 deg arc, radius 120-140 ft wandering, 15 min; then 2 x (20 ft straight drift in 60 s + a new 10 deg arc, 15 min). 1 Hz, 1.0 m GPS noise, 47 min. ASSUMED parameters in the script header | Never locks (arc < 40 deg; the fit collapses to a 3-9 m radius) -> CONSERVATIVE at 15:00, alarm circle 143 m (2 x 64 m reach + 15 m) round the centroid. The 40 ft total drag peaks ~12 m from the centroid: **no ALARM, no DRAG?** |
| `synth/drift_scenario_1008.csv` | same script (v2) | The same track + a heading on every fix (bow on the anchor, +8 deg constant compass offset, 2 deg noise) | v4 heading lock (ANC-40): LOCK (heading) at 13:00, anchor ~6 m off, alarm circle 66 m; the 40 ft of drag stays inside it (no ALARM) |
