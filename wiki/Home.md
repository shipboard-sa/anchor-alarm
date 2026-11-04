<!--
Purpose: Front page of the anchor-box design wiki - what the project is, where things are, page index.
Version: v11 (2026-10-23) - v11: live version v0.13.0 (since 10-21 20:44 CT); Next = what is left after the push; decisions to ANC-48; path D:\boat\AnchorAlarm. v10 (2026-10-21) - v10: live version v0.12.1; Next section (ANQ regroup, 10-21). v9 (2026-10-18) - v9: EXPECTED? cold-box question (ANC-45); decisions to ANC-45. v8 (2026-10-11) - v8: HOT WATER button (ANC-43); decisions to ANC-43. v7 (2026-10-09) - v7: FILL button on WATCH + FILL page (tank-fill taps, ANC-41); decisions to ANC-41. v6 (2026-10-07) - v6: Interfaces, Data-and-Config, Operations pages; live version line; decisions to ANC-39 (rebuild-from-spec pass). v5 (2026-10-07) - v5: decisions to ANC-31. v4 (2026-10-06) - v4: night-watch bar (Display-and-UI), Pi watch on; decisions to ANC-30. v3: Monitoring-Hosts page (DO1 -> AL-13, ANC-25). v2: the box is LIVE (v0.4.0); page index updated. v1: project split out
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki (D:\boat\AnchorAlarm\wiki, was code/anchor-box/wiki). Current state lives here and in the pages below;
  status / open items in ../CHECKPOINT.md; requirements in ../SPEC.md.
Invocation: n/a (reference doc)
-->

# Anchor box - design wiki

The anchor box is the touch screen at the nav station (foot of the companionway ladder - 10-06) that watches the anchor. It runs a **clean-room firmware**
(ANC-01) built on the owner's own design: fit a circle to the boat's swing, the centre is the anchor, and alarm when
the boat leaves the circle plus 15 m (ANC-02, ANC-10). Between alarms the screen is a large red 24-hour clock in
boat time (ANC-05). **Live on the box since 2026-10-02** (now v0.13.0, 10-21 - see CHECKPOINT for the running version),
updated by signed OTA over Wi-Fi; its alarm reaches the CAMS panel's Anchor Drag (AL-12) through DO0. Besides the
anchor watch it lights the panel's Automation lamp (AL-13) through DO1 when the Pi or the Cerbo goes quiet, is the
night-watch switch, shows (and acks) the rules engine's P1 / P2 alarms, starts / stops tank-fill sessions (FILL button on WATCH) starts a one-off hot-water run (HOT WATER button on WATCH), arms the slip fence (DOCK / LEAVE), asks the engine's cold-box 'EXPECTED?' question (YES / NO), offers RESET on an engine alarm the engine says may be reset, and shows the underway WATCH CHECK-IN (tap = I'M UP). It shows immediate-action events only (ANC-48).

It is a **separate project** (ANC-14; private GitHub repo), not part of Ship's Automation. Ship's Automation reads the
box's wired alarm contact, and serves the box night-watch state, alerts, tank-fill sessions, the hot-water run, boat mode (DOCK / LEAVE), the cold-box question, crew RESET and the watchkeeper timer over the LAN ([Interfaces](Interfaces.md)).

## Next (10-23; detail in CHECKPOINT Open)

* **Before departure (10-21):** ANQ-18 forced full-ALARM test (faked drag -> ALARM -> DO0 -> AL-12 -> MUTE ->
  DISARM, panel muted); ANQ-21 off-boat copies of the original firmware image + the signing key (password manager).
* **v0.13.0 proofs (ANQ-40):** RESET on a test alarm; WATCH CHECK-IN underway (ANQ-27). Batch 2 is on the box since
  10-21 20:44 CT; 2.3 (AIS on the box) closed by ANC-48; router age (ANQ-32) seen live 10-23.
* **Parked:** ANQ-34 heading lock (until the CCU compass, F-226, is calibrated), ANQ-03, 04, 19, 17, 12, 08.
  **Open:** ANQ-06 reword / close mx-inv #900 (mx-inv is back up).

## Where things are

| Path | What |
|---|---|
| `../CHECKPOINT.md` | START HERE - state, how to build / test / update, gotchas, open items (ANQ-nn) |
| `../SPEC.md` | Requirements - every item marked LEE / DESIGN / ASSUMED |
| `../README.md` | Overview, update procedure, file map |
| `../main/` | ESP32 firmware (ESP-IDF v6.0.3 + LVGL 9.2) |
| `../reference/` | Python reference model + tests (the C ports must pass the same cases) |
| `../test_host/`, `../tools/` | PC tests of the C ports; OTA push + vector generators |
| `../docs/mockups/` | Clock mock-ups |

## Pages

| Page | Covers |
|---|---|
| [Architecture](Architecture.md) | Layers, files, data in / out, Wi-Fi, NVS, the boundary with Ship's Automation and CAMS |
| [Interfaces](Interfaces.md) | Every contract: HTTP served (status page line by line, OTA, test endpoints), SignalK + rules-engine calls with shapes / timeouts / errors, N2K PGNs decoded, wired outputs |
| [Data-and-Config](Data-and-Config.md) | Flash partitions, NVS keys + blob layouts, sdkconfig layers and every Kconfig option, secrets by name, RAM and tasks |
| [Operations](Operations.md) | Checking the box, failure modes with what still works, OTA slots + rollback, bricked-OTA / lost-key recovery |
| [Anchor-Watch-Algorithm](Anchor-Watch-Algorithm.md) | Circle fit, lock rule, CONSERVATIVE, coarse guard, DRAG?, power cut, context |
| [Display-and-UI](Display-and-UI.md) | CLOCK, WATCH, ALERT (engine alarms, tap to ack), buttons by state, ARM / DISARM / RE-ARM / ZONE / PLOT, ALARM banner, night-watch bar, FILL page, HOT WATER button, EXPECTED? screen, RESET on ALERT, WATCH CHECK-IN |
| [Time-and-Zones](Time-and-Zones.md) | GPS time acceptance, zone table + US DST, pin, night |
| [Monitoring-Hosts](Monitoring-Hosts.md) | DO1 -> panel AL-13: the Pi (SignalK) and Cerbo (N2K) watch, attribution, release, tests |
| [Outputs-and-Alarm](Outputs-and-Alarm.md) | DO0 to the panel, the circuit, power-up safety, the wiring test |
| [Hardware](Hardware.md) | The board, every pin / net / chip fact and its source; power and terminals as installed |
| [Build-Flash-Test](Build-Flash-Test.md) | Tests, release + OTA, bench / test builds, USB, what the first flash found |
| [Design-Decisions](Design-Decisions.md) | ANC-01 .. ANC-48 |
