<!--
Purpose: The box's second job - the 'monitoring host down' watch: how it decides the Pi or the Cerbo is down,
  what it shows, when DO1 (panel AL-13 'Automation') turns on and off, and how it is tested.
Version: v5 (2026-10-23) - v5: router age live in v0.13.0 (ANQ-32 closed). v4 (2026-10-21) - v4: bus-silent = no lamp confirmed (ANQ-26, 10-21); ANQ-24 / ANQ-32 status. v3 (2026-10-07) - v3: Pi side live since v0.6.0 (was 'built, not pushed'); ANQ-29 closed; per-host state
  machine (rebuild-from-spec pass). v2 (2026-10-06) - v2: Pi side ON (ANQ-25, ANC-29); status-page dark spells hardened (ANQ-29, ANC-30). v1 (2026-10-03)
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page; code in main/hostwatch.c (decisions), main/hostwatch_ctl.c (polling, DO1),
  main/n2k.c (Cerbo addresses); spec in reference/anchorwatch/hostwatch.py. Origin: Ship's Automation OQ-117 1b + 1c.
Invocation: n/a (reference doc)
-->

# Monitoring hosts (DO1 -> panel AL-13 'Automation')

The owner asked for this on 2026-10-03 (Ship's Automation OQ-117, options 1b + 1c). It takes the place of the Finder timer
relay (Q26) that was put off for cost. The box watches two of the boat's monitoring hosts and lights the panel's
**Automation** lamp (AL-13) when one of them stops. It informs only. It controls nothing, sends nothing to the
Cerbo and never transmits on N2K (ANC-03). The anchor watch stays the box's first job: this runs in its own
low-priority task, and every blocking call (HTTP, ping, flash) happens there (ANC-25).

## What it watches

| Host | How | Down when | Screen text |
|---|---|---|---|
| **Pi** (SignalK) - **LIVE since v0.6.0, 10-06** (ANC-29; was off, ANC-26) | Wi-Fi, every 10 s: `GET http://192.168.1.10:3000/signalk/v1/api/vessels/self/automation/health/healthy`. Healthy = every boolean `value` true AND the newest `timestamp` fresh (< 90 s). If that path answers 404, fall back to `.../navigation/position` with a fresh timestamp (< 60 s) = "Pi + SignalK alive" | unhealthy 60 s, **and the router 192.168.1.1 answers a ping** | `Pi / SignalK not answering` |
| | | unhealthy 60 s and the router silent -> **no lamp** (a router outage is not a Pi fault) | `Network down - router not answering` |
| **Cerbo** (N2K) | any frame from one of its N2K addresses (the Cerbo and the virtual devices it puts on N2K: tanks 208-211, MPPTs, MultiPlus, shunts, system - Ship's Automation cerbo-victron.md). PGN 126993 heartbeat (~60 s) means there is always one to hear | silent 180 s while other sources are still heard | `Cerbo silent on N2K` |
| | | nothing from ANYONE for 10 s -> **no lamp** (the bus or the box's CAN, not the Cerbo - design confirmed by 10-21, ANQ-26) | `N2K bus silent` |

**Release:** a lit lamp goes out only after its device has been healthy again for 30 s, never just because the
router or bus check changed. One bad poll during recovery restarts the 30 s (no flapping). DO1 = Pi OR Cerbo.

**Fresh** = age measured against the box's own GPS UTC (the MSC-10 time, n2k.c). With no GPS time yet, the rule
is "the timestamp changed within the limit". The health limit is 90 s, not 60 s, because the engine republishes
`automation.health.*` on change plus a 60 s keep-alive (Ship's Automation spec/10-Engine/README.md), so 60 s would flap.

## Which addresses are the Cerbo

The box learns them from **ISO Address Claims (PGN 60928)**. Every source whose NAME carries manufacturer code
**358 (Victron Energy)** counts as the Cerbo's. The table is keyed by NAME, so an address change after a Cerbo
reboot moves the entry, and a claim by another device at one of those addresses removes it. The box is
listen-only, so it can't ASK for claims. It hears them when the Cerbo (re)starts or when another node asks (the
Pi's SignalK and the plotters do at start-up). The table is kept in NVS (`anchorbox`/`cerbo_tab`) across the box's
own restarts. Until the first claim is heard: "no Victron address claim heard yet", and no lamp.

ASSUMED - check your wiring (ANQ-24 closed 10-21 on the 15 addresses seen 10-05; this point was not separately checked): every Victron source on the bus comes through the Cerbo (no native VE.Can
device that would keep talking with the Cerbo dead). Ship's Automation counts 15 Victron sources, all bridged by the
Cerbo (cerbo-victron.md device map).

## Where to see it

* **CLOCK** face: the text under the digits, in the same red (a coloured screen stays reserved for the anchor
  ALARM, ANC-06). No forced wake: it is not an anchor alarm.
* **WATCH** screen: `AUTOMATION: ...` above the buttons.
* **Status page** (http://192.168.1.100/): `monitor DO1 on/off | last: <event>`, the Pi line (state, last result,
  HTTP code, path, age, router) and the Cerbo line (addresses, last frame, source). Free internal RAM too. The
  `router` word is the LAST ping, which only runs while the Pi looks unhealthy - it can be hours old, so since v0.13.0 it carries its age (`router answered 9 h 17 min ago`, ANQ-32 closed 10-23).

## Settings (Kconfig "Monitoring-host watch", sdkconfig.defaults to change)

`ANCHORBOX_PI_WATCH` (Kconfig n, **y in sdkconfig.defaults** since 10-06), `PI_HOST`, `PI_PORT`, `PI_HEALTH_PATH`, `PI_FALLBACK_PATH`, `PI_POLL_S` 10,
`PI_HEALTH_FRESH_S` 90, `PI_FALLBACK_FRESH_S` 60, `PI_DOWN_AFTER_S` 60, `ROUTER_HOST`, `CERBO_SILENT_S` 180,
`CERBO_MFR` 358, `HOSTWATCH_RELEASE_S` 30. A change = a rebuild + signed OTA.

## Pi side ON (ANC-29, 10-06)

LIVE since v0.6.0 (10-06 16:49 CT): status `pi OK - healthy (HTTP 200, health path)` against our rebuilt Pi. The
release image polls `192.168.1.10:3000` every 10 s. With nothing answering there, DO1 / AL-13 lights about 60-70 s
after the box boots (router answering) - so a Pi that stops serving SignalK on :3000 lights the lamp, by design.

**Status page dark spells (ANQ-29, ANC-30).** Code reading 10-06: the router ping and the Pi GET run on the
hostwatch task and never hold a lock the HTTP server needs (`s_mux` is never held across I/O), and the ping session
cleans itself up (the ESP-IDF ping task exits within 1 s of delete) - so the ping cannot block the server directly.
What can: the lwIP socket pool was 10 and the HTTP server alone may hold 7 + 3 = 10, so every client socket (the
Pi GET, the ping's raw socket, now the night-watch GET / POST) competed with it; with the pool full a new connection
waits unanswered while the box runs on (= 'dark, uptime still counting'). Also no LRU purge (idle keep-alive tabs
hold server slots) and an OTA receive loop that retried timeouts for ever. Fixed: pool 16, LRU purge on, a stalled
OTA aborts after 30 s. **CLOSED 10-07 (ANQ-29):** the page answered at 15.9 h uptime with the Pi watch pinging all
night.

## The decision state machine (hostwatch.c = hostwatch.py)

States per host: `UNKNOWN` (nothing known yet), `OK`, `DEGRADED` (Pi unhealthy < 60 s), `DOWN` (lamp lit),
`NET_DOWN` (Pi: router silent / Cerbo: bus silent - no lamp). DO1 = Pi asserted OR Cerbo asserted.

**Pi** (evaluated after every poll, 10 s):

| Condition | Result |
|---|---|
| poll healthy, not asserted | OK (event "network back" if it was NET_DOWN) |
| poll healthy, asserted | stays DOWN until healthy continuously 30 s -> released, OK ("Pi healthy 30 s - DO1 released"); one bad poll restarts the 30 s |
| unhealthy < 60 s | DEGRADED |
| unhealthy >= 60 s, router ping answers | asserted, DOWN ("Pi DOWN - DO1 on") |
| unhealthy >= 60 s, router silent | NET_DOWN, no lamp ("network down (router silent) - not the Pi, no lamp"); pinged again on every poll |
| asserted, then the router goes silent | stays asserted (a lit lamp goes out only on a healthy Pi) |

**Cerbo** (evaluated every second from n2k.c's frame times):

| Condition | Result |
|---|---|
| no Victron address known yet | UNKNOWN, no lamp |
| a Cerbo frame within 180 s (counted from the later of the last Cerbo frame and when the table became known) | OK, or if asserted: released after 30 s of Cerbo frames ("Cerbo heard 30 s - DO1 released") |
| silent >= 180 s, another source heard within 10 s | asserted, DOWN ("Cerbo SILENT - DO1 on") |
| silent >= 180 s, nobody heard for 10 s | NET_DOWN, no lamp ("N2K bus silent - not the Cerbo, no lamp") |
| asserted, then the bus goes silent | stays asserted |

Events go to the serial log and the status page `last:`; the CLOCK / WATCH notice shows the DOWN / NET_DOWN text of
each host.

## Why the Pi side was off (ANC-26, superseded by ANC-29)

On 10-03 the Pi's SignalK no longer answered on :3000 (it did on 09-28/29). It is served only through the previous HTTPS
proxy (`signalk.your-pi-hostname.unladenswallow.us`, reachable by IP with that Host name). Polling :3000 would have
reported a healthy Pi as down. The owner chose: Pi side off until the cut-over, then on when our SignalK serves the
health path on :3000 (ANQ-25). No temporary TLS code for the previous automation stack.

## Tests

* `reference/tests/test_hostwatch.py` (13 cases) - the spec.
* `test_host/run.bat` -> `test_hostwatch.exe`: 11 SignalK-reply parse cases + 32 scenarios / 46,970 observations
  (scripted + seeded random outages) replayed against the Python reference - every event, state and lamp.
* Real replies checked 10-03: the previous SignalK position reply (two sources, different timestamps) parses to the
  newest timestamp.
* Aboard (ANQ-23): the test build `sdkconfig.hwtest` -> `POST /test/hostwatch?pi=down|ok&router=down|up&cerbo=silent&bus=silent`
  fakes the inputs (10 min, then real again); the thresholds are the real ones. With owner's yes and the panel
  muted: Cerbo silent -> 180 s -> DO1 -> AL-13 lamp -> `cerbo=ok` -> 30 s -> released; pi=down with the real
  router ping -> 60 s -> DO1; `router=down` -> no lamp. Unplug nothing live. Normal build straight back after.
  Done 10-05: all four cases proven (Outputs-and-Alarm).

## Not covered

Not fail-safe: a dead or unpowered box lights nothing (the Pi is to watch the box in return - Ship's Automation side).
The Cerbo check proves its N2K side is alive, not that its battery / VE.Bus monitoring is healthy. The router /
LAN is a common point for all Wi-Fi watchers (accepted, OQ-117).
