<!--
Purpose: How the anchor box is put together - firmware layers, files, what goes in and out, and the
  boundary with the boat's other systems (N2K, CAMS panel, Wi-Fi, Ship's Automation).
Version: v11 (2026-10-23) - v11: v0.13.0 in the layer map (wkeep.c / wkeep_ctl.c, RESET in alerts_ctl.c); nine engine contracts. v10 (2026-10-21) - v10: every engine contract listed in the boundary section; stale #900 pointer -> ANQ-06. v9 (2026-10-18) - v9: restock.c / restock_ctl.c / ui_ask.c - the EXPECTED? question (ANC-45). v8 (2026-10-11) - v8: hotwater.c / hotwater_ctl.c (ANC-43). v7 (2026-10-09) - v7: one engine link (engine_link.c) serves night watch / alerts / fill; LVGL pool in PSRAM (ANC-42). v6 (2026-10-09) - v6: tank-fill taps (fill.c / fill_ctl.c / ui_fill.c, ANC-41). v5 (2026-10-07) - v5: engine alerts (alerts.c / alerts_ctl.c / ui_alert.c, ANC-32/33) in both diagrams;
  S2-DI4 tap wired + proven 10-06; tasks + start-up timeline; test counts; links to Interfaces / Data-and-Config /
  Operations (rebuild-from-spec pass). v4 (2026-10-06) - v4: night-watch remote (nightwatch.c / nightwatch_ctl.c, ANC-27); Pi watch on (ANC-29). v3: monitoring-host watch (hostwatch / hostwatch_ctl, DO1 -> AL-13, n2k Cerbo addresses). v2: as live (v0.4.0): DO0 = panel, Wi-Fi + signed OTA + status page, n2k / boattime /
  anchorwatch / watch_ctl / boatclock layers, host tests. v1: step 2 plan
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page; file-level detail is in each file's header.
Invocation: n/a (reference doc)
-->

# Architecture

## On the boat

```
 NMEA 2000 backbone ──(LISTEN ONLY, CAN 15/16)──> ANCHOR BOX ──DO0──> CAMS Master Alarm Panel "Anchor Drag" (AL-12)
   MSC-10 (src 3): 126992 time, 129025 position     (ESP32-S3,        └─ tapped by the Pi on S2-DI4 (Ship's Automation WF-107, W05 wired + proven 10-06, F-202)
   power: the bus (Always On Bus)                     touch LCD)
                                                        │
                                   boat Wi-Fi 'YourBoatWiFi' (2.4 GHz) - 192.168.1.100 / anchor-box.local
                                        ├─ GET /       read-only status page (clock, watch, distance, last event)
                                        ├─ POST /ota   signed firmware images only (laptop: tools/ota_push.py)
                                        └─ the box's own client calls (to the Pi, 192.168.1.10):
                                             GET :3000 SignalK health (every 10 s)
                                             GET / POST :8200 /night-watch (every 5 s; POST on a tap)
                                             GET :8200 /alerts (every 2 s) + POST /ack on a tap
```

* **In:** position + UTC from the MSC-10 on N2K. The box never transmits on N2K (ANC-03; a transmitting alert test
  is parked, ANQ-17). Water speed / wind context is in the reference model, not yet in the C port.
* **Out:** DO0, an open-collector sink to board GND (Hardware), wired with GND to the panel's Anchor Drag input -
  the alarm reaches the panel with no Pi in the loop (ANC-17, proven 10-02). **DO1** = monitoring host down -> panel
  AL-13 'Automation' (ANC-25, [Monitoring-Hosts](Monitoring-Hosts.md); wired + proven 10-05). No buzzer.
* **Wi-Fi:** updates, the status page, polling the Pi's SignalK health (ON since the 10-06 build, ANC-29), and the
  night-watch remote: GET / POST `http://192.168.1.10:8200/night-watch` on the rules engine (ANC-27, bearer token
  from sdkconfig.secrets), and the engine alerts: GET `/alerts` every 2 s, POST `/ack` on a tap (ANC-32 / ANC-33). Every
  contract with shapes, timeouts and errors: [Interfaces](Interfaces.md). The watch never depends on it: a Wi-Fi failure is logged and the box carries on. Nothing
  on the network can control the ANCHOR watch (no remote ARM / DISARM - by design); the night-watch state is the
  engine's, the box only asks.

## Boundary with Ship's Automation (ANC-14)

Ship's Automation (the Pi) is a separate project. It sees the box's alarm as the S2-DI4 input (AL-12) and its WF-107
rule (the W05 tap is wired + proven 10-06, F-202). The box also reads the rules engine: the night-watch remote (D-118,
ANC-27), the engine alerts with ack (D-122, ANC-33), tank fills (D-129, ANC-41), the hot-water run (D-135, ANC-43),
DOCK / LEAVE (D-151, ANC-44), the cold-box question (D-128, ANC-45), crew RESET (D-163, ANC-46) and the watchkeeper
timer (D-161, ANC-47) - so Ship's Automation is the counterparty of nine box contracts, not only a reader of S2-DI4 ([Interfaces](Interfaces.md)). It does NOT run the anchor watch. The Pi can be
dead and the box still alarms on its own screen and on the panel. mx-inv task #900 still says
'DO1 -> AL-12' (Ship's Automation spec/04-Interfaces/devices.md:105 quotes it) - reword it there (ANQ-06).

## Firmware layers (main/)

```
 app_main.c ── start order: board -> display + screens -> n2k -> net (Wi-Fi/OTA, opens NVS) -> watch_ctl ->
 │             hostwatch_ctl -> nightwatch_ctl / alerts_ctl / fill_ctl (state only) -> engine_link -> boatclock
 │
 ├─ n2k.c ──────── TWAI listen-only 250k: 126992 -> bt_sync (GPS time acceptance), 129025 -> latest position,
 │                 60928 address claims -> the Cerbo's addresses (Victron NAMEs), last Cerbo / other frame time
 ├─ boattime.c ─── pure C: zones, US DST, nautical, steadiness, pin, sun -> night, clock face
 ├─ boatclock.c ── task 1 Hz: face -> ui_clock, anchor symbol, zone pin (NVS), status lines, expander refresh, health mark
 ├─ anchorwatch.c  pure C port of reference geo + fit + watch (+ snapshot / restore)
 ├─ hostwatch.c ── pure C: Pi / Cerbo down decisions, attribution, 30 s release, SignalK reply parsing
 ├─ nightwatch.c ─ pure C: engine /night-watch reply parsing, what the CLOCK's night-watch bar shows
 ├─ engine_link.c  task "engine", priority 1, PSRAM stack: ONE keep-alive HTTP client + one 2 KB PSRAM reply buffer;
 │                 calls the four service steps below in turn, sleeps until the earliest is due or a tap kicks it (ANC-42)
 ├─ nightwatch_ctl.c service step: GET every 5 s, POST on a tap (bearer token), link state
 ├─ alerts.c ───── pure C: /alerts reply parsing, take-over / release / tap rules, CLOCK + WATCH lines (ANC-33)
 ├─ alerts_ctl.c ─ service step: GET /alerts every 2 s, POST /ack on a tap, POST /reset/<rule> on a confirmed RESET (ANC-46)
 ├─ fill.c ─────── pure C: /fill reply parsing (nested JSON), FILL page rows / tab / tap rules (ANC-41)
 ├─ fill_ctl.c ─── service step: GET /fill every 10 s (2 s with the page open), POST /fill on a tap
 ├─ hotwater.c ─── pure C: /hot-water reply + 409 parsing, HOT WATER button / notice faces, tap rule (ANC-43)
 ├─ hotwater_ctl.c service step: GET /hot-water every 10 s, POST /hot-water on a tap
 ├─ restock.c ──── pure C: /restock reply + 409 parsing, which box is asked, take-over / release / tap rules (ANC-45)
 ├─ restock_ctl.c  service step: GET /restock every 10 s, POST /restock YES / NO on a tap
 ├─ wkeep.c ────── pure C: /watch reply parsing, when the WATCH CHECK-IN shows, stage words, tap rule (ANC-47)
 ├─ wkeep_ctl.c ── service step: GET /watch every 5 s, POST /watch {action: up} on a tap
 ├─ hostwatch_ctl.c task 1 Hz, priority 2: Pi poll (HTTP, ON since 10-06) + router ping, Cerbo frame
 │                 times from n2k.c, DO1, notice text; Cerbo address table -> NVS 'cerbo_tab'
 ├─ watch_ctl.c ── task 1 Hz: newest fix -> watch, DO0 = ALARM, screen wake, circle -> NVS on every state change,
 │                 restored at power-up (no age limit), breadcrumbs (6 h, PSRAM), RE-ARM
 ├─ net.c ──────── Wi-Fi STA (non-fatal), mDNS, HTTP: status page + signed OTA (refused while armed / ALARM)
 ├─ screens.c ──── CLOCK / WATCH / ALERT rules (wake tap swallowed, 2-min fallback, ALARM holds WATCH)
 │                 ui_clock.c (seven-segment HH:MM, anchor symbol, 'M', night red, night-watch bar)
 │                 ui_watch.c (state, buttons by state, ARM / DISARM / RE-ARM / ZONE pages, PLOT, ALARM banner)
 │                 ui_alert.c (the ALERT screen: engine P1 / P2 alarms, tap to ack)
 ├─ display.c ──── LVGL 9.2: partial render (2 x 20-line buffers, internal DMA RAM) -> panel PSRAM frame buffer; touch
 └─ board.c ────── I2C, CH422G (outputs OFF first, rewritten every 5 s), RGB panel, touch reset, backlight, DO0/DO1
                   ch422g.c, gt911.c (own drivers from the datasheets)
```

Locking: LVGL runs in its own task under `display_lock()`; `watch_ctl` never takes the display lock while holding
its own mutex (screen actions run after the mutex is released), so the UI and the watch cannot deadlock.

### Tasks and timing

| Task | Priority | Job | Never blocks on |
|---|---|---|---|
| n2k | 5 | decode frames from the ISR ring | - |
| ui (LVGL, core 1) | 4 | draw, touch, the 1 s screen-rules timer (screens.c) | network |
| watch | 4 | 1 Hz: newest fix -> watch -> DO0, NVS snap on state change | network, display lock (screen actions after its mutex is released, 500 ms lock timeout) |
| clock | 3 | 1 Hz: CLOCK face, status page text, expander refresh (5 s), health mark (30 s), N2K log (60 s) | network (display lock 100 ms timeout) |
| hostwatch | 2 | Cerbo check 1 Hz, Pi poll 10 s (3 s HTTP + up to 6 s ping), DO1 | - |
| engine | 1 | the one rules-engine link: night watch, alerts (+ RESET), fill, hot-water, mode, restock, watchkeeper polls / taps in turn (PSRAM stack) | - |
| httpd | 5 | status page, OTA | - |

Every blocking network call lives on priority 1-2 tasks; the watch (4) can always run. Data between tasks: n2k
state behind a mutex (copied out by `n2k_get`), watch behind its own mutex, night watch / alerts views behind
spinlocks, copied whole.

Start-up timeline: outputs OFF at once (CH422G default + first write); display and CLOCK ("--:--") within the first
second; circle restored before the watch task starts; Wi-Fi joins in the background; night-watch poll from 12 s,
alerts from 14 s, Pi poll from 15 s; clock shows time after two agreeing GPS time messages and a position (zone);
image marked good at 30 s.

Persistent state (NVS namespace `anchorbox`): `armed`, `rode`, `snap` (the circle, `aw_snapshot_t`), `zone_pin`,
`cerbo_tab` (the Cerbo's N2K addresses + NAMEs) - layouts: [Data-and-Config](Data-and-Config.md).
Flash: 16 MB - ota_0 / ota_1 (4 MB each, signed images, rollback), otadata, NVS 24 KB, phy_init.

## Reference model and tests

`reference/` (Python `anchorwatch`) is the executable spec: `watch.py` (state machine + snapshot / restore), `fit.py`,
`geo.py`, `boattime.py`, `sim.py`, `replay.py`, `service.py`. 163 tests (pytest, offline). Behaviour changes land
there first. `tools/make_c_vectors.py` and `tools/make_watch_vectors.py` turn its cases into test vectors;
`test_host/run.bat` compiles the C ports on the PC (zig cc) and checks them: 14,372 boat-time checks, 20 watch
scenarios (events at the same second, same final circle), plus the hostwatch, nightwatch, alerts, fill, hotwater, boatmode and restock host tests -
9 host test programs in all (inventory: [Build-Flash-Test](Build-Flash-Test.md)).

See also: [Interfaces](Interfaces.md) (every contract), [Data-and-Config](Data-and-Config.md) (flash, NVS,
sdkconfig), [Operations](Operations.md) (failure modes, rollback).
