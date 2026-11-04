<!--
Purpose: Every interface of the anchor box in one place - what it serves on HTTP, what it asks of other hosts
  (SignalK, rules engine), what it hears on NMEA 2000, its wired outputs - with shapes, timeouts and error behaviour.
Version: v6 (2026-10-23) - v6: v0.13.0 contracts added from main/*.c: GET / POST /watch, POST /reset/<rule>, status lines boat mode / restock / watchkeeper, router age live. v5 (2026-10-21) - v5: router-age line queued (ANQ-32, Build-Queue 2.4). v4 (2026-10-18) - v4: GET / POST /restock + status line (ANC-45, Ship's Automation WF-502 / D-128). v3 (2026-10-15) - v3: GET / POST /mode + status line (ANC-44, Ship's Automation D-151). v2 (2026-10-11) - v2: GET / POST /hot-water + status line (ANC-43). v1 (2026-10-07) - v1: new page (rebuild-from-spec pass); facts read from main/*.c at v0.8.0.
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page. Code: net.c (server), hostwatch_ctl.c / hostwatch.c (SignalK + ping),
  nightwatch_ctl.c / nightwatch.c and alerts_ctl.c / alerts.c (rules engine), n2k.c (bus), board.c (outputs).
  Counterpart contract for the engine side: Ship's Automation spec/04-Interfaces/anchor-box-alerts.md and
  spec/10-Engine/README.md (API table).
Invocation: n/a (reference doc)
-->

# Interfaces

The box has four kinds of interface: an HTTP server on the boat Wi-Fi, HTTP / ICMP clients towards the Pi
(boat-automation, 192.168.1.10), a listen-only NMEA 2000 port, and two wired open-collector outputs to the CAMS
Master Alarm Panel. None of the network interfaces can arm, disarm or change the ANCHOR watch (by design, ANC-27).
The anchor watch needs only the N2K port and DO0.

## Network identity

| Item | Value | Source |
|---|---|---|
| Wi-Fi | STA on 'YourBoatWiFi' (2.4 GHz). SSID / password compiled in from `sdkconfig.secrets` (`CONFIG_ANCHORBOX_WIFI_SSID` / `_PASSWORD`); `esp_wifi_set_storage(WIFI_STORAGE_RAM)` - the login is NOT kept in NVS | net.c; SPEC 8.6 |
| Address | 192.168.1.100 by DHCP reservation 'AnchorAlarm' on the boat router (GL-X3000), MAC XX:XX:XX:XX:XX:XX | Ship's Automation spec/04-Interfaces/network.md:62; Build-Flash-Test (first flash) |
| Names | mDNS `anchor-box.local`, service `_http._tcp` port 80; lwIP hostname `anchor-box` | net.c, sdkconfig.defaults |
| Reconnect | on every disconnect the box calls `esp_wifi_connect()` again; reason logged every 10th time (201 = network not found, 15 / 204 = wrong password) | net.c |
| No Wi-Fi configured | empty SSID -> Wi-Fi off; status page, OTA, Pi watch, night watch and alerts all off; clock + anchor watch + Cerbo watch unaffected | net.c, alerts_ctl.c, nightwatch_ctl.c |

If the router hands out a different address (seen 10-07 before the reservation existed: .189), nothing on the box
breaks, but `ota_push.py --host 192.168.1.100` and the engine's box-reachability rule (WF-004-BOX) miss it until
the box reboots onto its reservation (CHECKPOINT ANQ-32).

## Served by the box - HTTP on port 80 (esp_http_server)

Server config: max 7 open sockets (IDF default), `lru_purge_enable` (oldest idle connection dropped when all 7 are
held), `recv_wait_timeout` 10 s, task stack 12 KB (the RSA-3072 signature check runs on it; 4 KB overflowed 10-02).
lwIP socket pool 16 (`CONFIG_LWIP_MAX_SOCKETS`) so the server cannot starve the client sockets (ANC-30). One
request at a time (single server task).

| Method + path | Build | Request | Response | Errors |
|---|---|---|---|---|
| `GET /` | all | - | 200 `text/plain`, the status text (format below) | 500 `no memory` if the 2 KB PSRAM buffer cannot be had |
| `POST /ota` | all | body = the raw signed app image (`build\anchor_box.bin`), `Content-Length` required | 200 `OK - signature verified, restarting` then a restart 1 s later into the new slot | 409 `update already running`; 409 `watch armed or alarm showing` (the update gate, ANC-24); 400 `bad image size` (0 or larger than the slot); 400 `update refused: <esp_err name>` - e.g. `ESP_ERR_OTA_VALIDATE_FAILED` for a wrong signature / bad image, `ESP_ERR_TIMEOUT` when the upload stalls 30 s (3 x 10 s receive timeouts) |
| `POST /test/output?do=0\|1&s=1..15` | **outtest only** (`CONFIG_ANCHORBOX_REMOTE_OUTPUT_TEST`) | query only | 200 `DO<n> ON for <s> s`; both outputs forced OFF when the timer ends | 400 `do=0\|1, s=1..15`. 404 on every release build |
| `POST /test/hostwatch?pi=down\|ok&router=down\|up&cerbo=silent&bus=silent` | **hwtest only** (`CONFIG_ANCHORBOX_HOSTWATCH_TEST`) | query only; any key not given stays as it was | 200 `fake pi <n> router <n> cerbo <n> bus <n> for 10 min`; fakes expire 10 min after the last call | 404 on every release build |

During `POST /ota` both outputs are forced OFF (SPEC 8.4); the watch cannot be armed at the time (gate), so DO0 is
off anyway. Status page is read-only by design - there is no write endpoint except OTA.

### Status page text (GET /) - line by line

```
anchor-box
version <git describe of the build, e.g. v0.8.0>
built <compile date> <compile time>
running ota_0 | ota_1
ip <a.b.c.d> | -
uptime <s> s
internal RAM free <KB> KB (lowest <KB> KB)
update allowed | BLOCKED (watch armed / alarm)
clock HH:MM <zone id> (auto|PINNED) day|NIGHT        | clock --:-- (no GPS time yet|no position yet) day
watch <OFF|ACQUIRING|LOCKED|CONSERVATIVE|ALARM>[ armed m:ss rode <ft> ft gps ok|NONE[ <d> m of <r> m | <d> m from the provisional centre][ DRAG?][ MUTED][ GPS-STALE]]
[not locked: <reason>]                                (ACQUIRING / CONSERVATIVE only)
last <newest watch event text | ->
monitor DO1 ON|off | last: <host-watch event>
pi <OK|UNKNOWN|DEGRADED|DOWN|NET_DOWN> - <healthy|health value false|stale timestamp|SignalK answers, neither path published|HTTP error|no answer|not polled yet>[ (HTTP <code>, health|fallback position path[, <s> s old])][, router answered|SILENT <n> s|<n> min|<h> h <mm> min ago]
cerbo <state> - no Victron address claim heard yet | <n> addr <sa> <sa> ...[, last frame <s> s ago (src <sa>)]
night watch ARMED|OFF[ NUDGE][ night] (by <who>, since <iso>) - link ok, last answer <s> s ago, token set|MISSING[, SENDING][ | <error>][ | last tap HTTP <code>]
            | night watch: NO LINK|not polled yet - engine <host>:<port>, last answer <age> (HTTP <code>), token ...
alerts seq <n>: <total> active, <unacked> not acked[, HORNS][, top WF-nnn] - link ok, last answer <s> s ago | ack token set|MISSING[, last tap ACK ok|FAILED]
            | alerts: NO LINK|not polled yet - engine <host>:<port>/alerts, last answer <age> (HTTP <code>) | ...
fill: <tank> tap|watermaker[ waiting] (<by>) ...| no sessions, watermaker RUNNING|off - link ok, last answer <s> s ago, token set|MISSING[, SENDING][ | <error>][ | last tap HTTP <code>]
            | fill: NO LINK|not polled yet - engine <host>:<port>, last answer <age> (HTTP <code>), token ...
hot water: off|HEATING|on (no draw yet)|HELD (lockout / inverter shed), heater <A>|?, bank <%>|? (floor <%>), last run -|hot|no draw|time limit|stopped - link ok, <s> s ago, token set|MISSING[, SENDING][ | <message>][ | <s> s left][ | last tap HTTP <code>]
            | hot water: NO LINK|not polled yet - engine <host>:<port> (HTTP <code>), token ...
boat mode: <none|DOCK|ANCHOR|MOTOR|SAIL|AWAY>[ (secured)], <n> crew phone(s) aboard, leave ok|NOT ok - link ok, <s> s ago, token set|MISSING[, SENDING][ | <message>][ | last tap HTTP <code>]
restock: none asking|ASKING <LABEL> (<n> asking), marks none|<box> hold|NO, ..., hold <h> h - link ok, <s> s ago, token set|MISSING[, SENDING][ | <message>][ | last answer HTTP <code>]
watchkeeper: off (<mode>)|ON stage <n>, <m> min since I'm up, next check-in <s> s - link ok, <s> s ago, token set|MISSING[, SENDING][ | <message>][ | last tap HTTP <code>]
            | (each of the three: NO LINK|not polled yet - engine <host>:<port> (HTTP <code>), token ...)
[TEST FAKES ACTIVE: pi <n> router <n> cerbo <n> bus <n>]   (hwtest build only)
```

The app part (from `clock` on) is rebuilt once a second by the clock task (boatclock.c) into a 1.5 KB buffer; if it
fills, the later lines are cut. `router` is the LAST router ping, which only runs while the Pi looks unhealthy,
so it carries its age (ANQ-32, v0.13.0; seen live 10-23 `router answered 9 h 17 min ago`).

## Asked of the Pi (boat-automation, `CONFIG_ANCHORBOX_PI_HOST` = 192.168.1.10)

All client calls run on their own low-priority tasks (hostwatch 2, nightwatch 1, alerts 1) with PSRAM buffers; the
anchor watch (priority 4) never waits on them. Every call: `esp_http_client`, no redirects followed, plain HTTP.

### SignalK health (monitoring-host watch, ANC-25 / ANC-29) - port `PI_PORT` 3000

| Step | Request | Healthy when | Else |
|---|---|---|---|
| 1 | `GET /signalk/v1/api/vessels/self/automation/health/healthy` every `PI_POLL_S` (10 s), 3 s timeout, 2 KB body | HTTP 200, every boolean `"value"` in the reply is `true`, AND the newest `"timestamp"` is fresh (< `PI_HEALTH_FRESH_S` 90 s) | 200 but a false value -> `health value false`; stale -> `stale timestamp`; other HTTP -> `HTTP error`; no answer -> `no answer` |
| 2 (only on 404 from 1) | `GET /signalk/v1/api/vessels/self/navigation/position` | HTTP 200 + newest `"timestamp"` fresh (< `PI_FALLBACK_FRESH_S` 60 s) | 404 -> `SignalK answers, neither path published` |
| 3 (only after 60 s unhealthy) | ICMP echo to `ROUTER_HOST` 192.168.1.1: 3 pings, 300 ms apart, 1 s timeout each, 6 s overall | any reply = router answers -> Pi DOWN, DO1 on | no reply = network down, no lamp |

"Fresh" = the reply timestamp (ISO-8601 `YYYY-MM-DDTHH:MM:SS(.sss)Z`, parsed by `hw_parse_iso_ms`) compared with the
box's own GPS UTC; a timestamp more than the limit in the FUTURE also counts as not fresh. With no GPS time yet:
"the timestamp value changed within the limit" (box uptime). The parser takes the newest timestamp in the reply
(a path can carry one per source). Decision rules: [Monitoring-Hosts](Monitoring-Hosts.md).

### Rules engine (port `ENGINE_PORT` 8200) - night watch, alerts, tank fills

Auth: one bearer token, `CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN` (`sdkconfig.secrets` only, compiled into the image -
so `build*/` stays private too). **All four calls below go over ONE keep-alive HTTP connection** from one task
(engine_link.c, ANC-42), one request at a time; a connection the engine dropped is retried once on a fresh one.
On the engine side it is the narrow token (`api.night_token_env`) that may only
`POST /night-watch`, `POST /ack`, `POST /fill`, `POST /hot-water`, `POST /mode` DOCK / MOTOR only, `POST /restock`, `POST /watch` I'M UP only and `POST /reset/<rule>` (only with engine.yml `api.box_reset: true`, `clear: manual` rules whose input reads normal, never `force`) (Ship's Automation D-118, D-122, D-129, D-135, D-151, D-128, D-161, D-163). The token is never logged or shown; the status
page says only `token set` / `MISSING`. Empty token = the box shows state but cannot send.

| Call | When | Request | Reply the box needs | Box behaviour on failure |
|---|---|---|---|---|
| `GET /night-watch` | every `NIGHT_WATCH_POLL_S` 5 s (first at 12 s uptime), 2 s timeout, 2 KB shared body, no auth | - | 200 JSON `{"armed": bool, "since": "ISO" \| null, "by": "str", "nudge": bool, "night": bool}` - `armed` required, the rest default false / empty | 2 failed polls in a row -> link lost -> bar 'Night watch: no link', not tappable |
| `POST /night-watch` | a tap on the bar (only while linked + token set) | `Content-Type: application/json`, `Authorization: Bearer <token>`, body `{"armed": <opposite of the engine's last state>, "by": "anchor-box"}`, 4 s timeout | 200 + the same JSON as GET (becomes the shown state) | 401 `not changed: token refused (401)`; 503 `not changed: engine has no token (503)`; no answer `not changed: engine not answering`; 200 unparsable `bad reply`; other `engine said HTTP n` - shown 30 s, state unchanged, re-poll at once |
| `GET /alerts?max=4` | every `ALERTS_POLL_S` 2 s (first at 12 s uptime), 2 s timeout, 2 KB shared body, no auth | - | 200 JSON `{"seq": n, "total": n, "unacked": n, "sounding": bool, "alerts": [{"id", "sev", "acked", "sim", "age_s", "text"}]}` - `seq` and `alerts` required; extra keys ignored; at most 4 items kept; text up to 340 chars (box buffer 344), escapes resolved, non-ASCII -> `?` | 2 failed polls -> link lost -> ALERT screen leaves, CLOCK line `Alerts: NO LINK to the rules engine` (never blank) |
| `POST /ack` | a tap on the ALERT screen while something is unacked, linked, token set | headers as above, body `{"by": "anchor-box"}`, 4 s timeout; then an immediate GET /alerts | 200 (body not used) | 401 / 503 / no answer / other -> `ACK not sent: ...` for 30 s |
| `GET /fill` | every `FILL_POLL_S` 10 s, every 2 s while the FILL page is open (first at 12 s uptime), 2 s timeout, 2 KB shared body, no auth | - | 200 JSON `{"tanks": {water_port \| water_stbd \| fuel_port \| fuel_stbd: {"label", "level": n \| null, "session": null \| {"source": "tap" \| "watermaker", "since", "by", "active"}}}, "watermaker": {"running", "watts": n \| null}}` - at least one known tank required; unknown tanks / keys ignored; a tank cut off by the buffer is dropped | 2 failed polls -> link lost -> FILL page `No link: ...` (404 named: engine before api.py v9), no rows; tab plain `FILL` |
| `POST /fill` | a FILLING / STOP tap on the FILL page (linked, token set, not a watermaker session) | headers as above, body `{"tank": "<key>", "on": true \| false, "by": "anchor-box"}`, 4 s timeout | 200 + the GET /fill view (becomes the shown state) | 400 `engine doesn't know that tank`; 401; 404 `engine has no /fill yet`; 503; no answer; bad reply; other - shown 30 s, state unchanged, re-poll at once |
| `GET /hot-water` | every `HOT_WATER_POLL_S` 10 s (first at 12 s uptime), 2 s timeout, 2 KB shared body, no auth | - | 200 JSON `{"on": bool, "state": "off" \| "heating" \| "on_no_draw" \| "held", "remaining_s": n, "amps": n \| null, "soc": n \| null, "soc_floor": n, "last": null \| {"result": "hot" \| "no_draw" \| "time" \| "stopped", "age_s": n, ...}, ...}` (Ship's Automation services-api.md) | 2 failed polls -> no link: the button reads HOT WATER, never claims a run |
| `POST /hot-water` | a tap on HOT WATER (linked, token set, no tap on its way) | headers as above, body `{"on": <opposite of the engine's last state>, "by": "anchor-box"}`, 4 s timeout | 200 + the GET view (shown at once); **409 `{"detail": "<reason>"}`** = refused (bank below the floor / SoC unknown) - the reason is shown | state unchanged; the reason or the HTTP error on the notice line for 30 s |
| `GET /mode` | every `MODE_POLL_S` 10 s, and at once after a tap, 2 s timeout, 2 KB shared body, no auth | - | 200 JSON `{"mode": null \| "DOCK" \| "ANCHOR" \| "MOTOR" \| "SAIL" \| "AWAY", "secured": bool, "crew_aboard": ["crew.skipper_cell", ...], "leave_ok": bool, "box_modes": ["DOCK", "MOTOR"]}` - mode, secured and leave_ok required; an unknown mode = not a reply (engine api.py v12) | 2 failed polls -> no link: no AT DOCK, ARM as normal, DOCK dark; 405 = engine before api.py v12 |
| `POST /mode` | DOCK on the ARM page / YES - LEAVE (linked, token set, no tap on its way, bm_dock_ok / bm_leave_ok) | headers as above, body `{"mode": "DOCK" \| "MOTOR", "by": "anchor-box"}` (the engine tags it `api:anchor-box` itself), 4 s timeout; then an immediate GET /mode | 200 `{"mode", "manual"}` (not shown - the GET is); **403 `{"detail": "DOCK armed, no crew phone aboard - end it from the phone or cockpit tablet"}`** = refused | the reason or the HTTP error on WATCH's second line for 30 s; state unchanged |
| `GET /restock` | every `RESTOCK_POLL_S` 10 s (first at 12 s uptime), 2 s timeout, 2 KB shared body (~0.45 KB idle, < 1.5 KB with four marks), no auth | - | 200 JSON `{"boxes": {fridge \| engel \| fz_aft \| fz_fwd: {"label", "ask_rule", "asking": bool, "alarm": bool, "mark": null \| {"kind": "hold" \| "denied", "since", "until", "by", "source"}}}, "hold_h": n}` - `boxes` + `hold_h` > 0 required, a box needs `asking` + `alarm` bools; unknown boxes / keys ignored (engine api.py v14) | 2 failed polls -> no link: no EXPECTED? screen, nothing asked |
| `GET /watch` | every `WATCHKEEPER_POLL_S` 5 s, 2 s timeout, no auth (v0.13.0, ANC-47) | - | 200 JSON `{"on": bool, "stage": 0-3, "since_up_s": n, "next_prompt_s": n, "alarm_s": n, "mode": "str"}` - `on` + `stage` required (stage outside 0-3 = not a reply); `alarm_s` defaults to 1500; `on` false forces stage 0 | 2 failed polls -> no link: no WATCH CHECK-IN screen |
| `POST /watch` | a tap anywhere on WATCH CHECK-IN (linked, token set, stage >= 1) | headers as above, body `{"action": "up", "by": "anchor-box"}`, 4 s timeout (engine tags it `api:anchor-box`) | 200 + the GET view -> 'I'M UP taken - next check-in in N min' | 409 / 403 / 422 with `detail` = the engine's reason; 401 'token refused - use the cockpit tablet'; 404 / 405 'engine has no /watch yet'; 503; no answer 'tap again' - shown 30 s |
| `POST /reset/<rule>` | RESET on the confirm page, for the first alert the engine marks `rst: true` (v0.13.0, ANC-46) | headers as above, body `{"by": "anchor-box"}` (never `force`), 4 s timeout | 200 -> 'RESET <rule> done' | 409 / 403 / 404 with `detail` -> 'RESET refused: <why>'; 401 'RESET not allowed for this box - use the cockpit'; 503; no answer - shown 30 s |
| `POST /restock` | YES / NO on the EXPECTED? screen (linked, token set, that box asking, no answer on its way) | headers as above, body `{"box": "<key>", "answer": "yes" \| "no", "by": "anchor-box"}`, 4 s timeout | 200 + the GET view (its mark ends the question at once); **409 `{"detail": "the <BOX> alarm is already up - no restock hold ..."}`** = YES refused (a NO is still taken) | the reason or the HTTP error in the footer for 30 s (`not answered: ...`); the buttons stay; a taken answer reads `YES taken - the <BOX> alarm waits 2 h` / `NO taken - the <BOX> alarm is on now` for 5 s |

Engine-side meaning of an ack from the box: acks ALL active alarms, silences the horns, stops ack-or-kill
countdowns and ends a running test, exactly like the S2-DI6 button; logged as `api:anchor-box` (D-122, ANC-33,
risk accepted by the owner). Full engine contract: Ship's Automation spec/04-Interfaces/anchor-box-alerts.md.

## NMEA 2000 (CAN) - listen-only

| Item | Value |
|---|---|
| Controller | ESP32-S3 TWAI (on-chip), `esp_twai_onchip` driver, **listen-only** (`enable_listen_only`): never transmits, never ACKs a frame |
| Pins / transceiver | TX GPIO 15, RX GPIO 16, TJA1051; the board's 120-ohm terminator switch OFF (CHECKPOINT ANQ-12, metered) |
| Bit timing | 250 kbit/s, sample point 87.5 % |
| Frames | 29-bit (extended) only; 11-bit frames dropped. ISR copies into a 256-frame ring; the `n2k` task (priority 5) decodes. Overflow counted as `dropped` |
| PGN from ID | PGN = DP(bit 24) << 16 \| PF << 8 \| (PF >= 240 ? PS : 0); source = low byte. Single-frame decode only (no fast-packet reassembly) |

| PGN | Name | Fields used (little-endian) | Used for |
|---|---|---|---|
| 126992 | System Time | byte 1 low nibble = time source (0 = GPS); bytes 2-3 days since 1970 (u16, 0xFFFF n/a); bytes 4-7 time of day in 0.0001 s (u32, 0xFFFFFFFF n/a, must be < 864000000) | GPS time acceptance (`bt_sync_feed`, ANC-19): kind 0 only, >= 2026-01-01, two consecutive messages agreeing within 2 s. **Any source** is accepted; on this bus only the MSC-10 (src 3) sends it |
| 129025 | Position, Rapid Update | bytes 0-3 latitude, 4-7 longitude (i32, 1e-7 deg; 0x7FFFFFFF n/a) | the anchor watch fix (1 Hz, newest only), breadcrumbs, zone, night, plot. **Any source** - today only the MSC-10 (F-135) |
| 60928 | ISO Address Claim | the 64-bit NAME; manufacturer code = NAME bits 21-31 | the Cerbo address table: NAME with code 358 (Victron) = a Cerbo address (ANC-25) |
| all | - | source address + time of arrival | Cerbo alive (frame from a Cerbo address) vs bus alive (frame from anyone else); per PGN / source counts for the serial log every 60 s |

The box never sends an ISO Request, so it learns the Cerbo's claims only when another node asks or the Cerbo
restarts (table kept in NVS). A transmitting alert test from the box is parked (SPEC 7.5, ANQ-17).

## Wired outputs (to the CAMS Master Alarm Panel)

| Output | Electrical | Panel input | Asserted when |
|---|---|---|---|
| DO0 | open-collector NPN sink to board GND, 5-36 V, 450 mA, opto-driven from CH422G OC0 (low = ON) | AL-12 Anchor Drag (Aqualarm terminal 11 - Ship's Automation io-relay-master.md C1); tapped by the Pi on S2-DI4 (WF-107, wired + proven 10-06, io_master.yml:76) | anchor watch in ALARM, MUTE included; off on DISARM / ARM / RE-ARM (ANC-23) |
| DO1 | same, CH422G OC1 | AL-13 Automation (Aqualarm terminal 13) - wired + proven 10-05 | the Pi or the Cerbo is down (ANC-25); off after 30 s healthy |

Both OFF from power-up (CH422G OC pins power up high) and during OTA. The expander is rewritten every 5 s (ANC-16).
DI0 / DI1 are not read. Details: [Outputs-and-Alarm](Outputs-and-Alarm.md), [Hardware](Hardware.md).

## What the box does NOT offer

No remote ARM / DISARM / RE-ARM / MUTE / zone pin; no N2K transmit; no MQTT / Modbus / SignalK write; no TLS
(LAN-only plain HTTP); no syslog - the status page and the USB serial log are the only diagnostics.
