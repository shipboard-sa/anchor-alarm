<!--
Purpose: Running the anchor box day to day - how to check it, what each failure looks like, what still works, and
  how to recover (incl. a failed or bad OTA, ota_0 / ota_1 and rollback, a lost signing key, a dead box).
Version: v3 (2026-10-23) - v3: router age live (ANQ-32), RAM on v0.13.0. v2 (2026-10-21) - v2: RAM row closed (ANQ-33); ANQ-19 parked, ANQ-26 design confirmed, ANQ-32 age queued. v1 (2026-10-07) - v1: new page (rebuild-from-spec pass); behaviour read from main/*.c at v0.8.0,
  incidents from CHECKPOINT.md (ANQ-12, ANQ-29, ANQ-32, ANQ-33).
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page. Procedures it points to: Build-Flash-Test (OTA, USB), Interfaces (status page).
Invocation: n/a (reference doc)
-->

# Operations

## Checking the box

* **Look at the glass.** CLOCK shows the time ("--:--" = no verified GPS time or no position), the anchor symbol, 'M'
  if the zone is pinned, the monitoring notice, the alerts line, and (after dark) the night-watch bar.
* **Status page** http://192.168.1.100/ (or `python tools\ota_push.py --host 192.168.1.100 --status`). Line
  meanings: [Interfaces](Interfaces.md#status-page-text-get----line-by-line). The lines to read after any change:
  `running` (slot), `version`, `internal RAM free ... (lowest ...)`, `clock`, `watch`, `pi`, `cerbo`, `night watch`,
  `alerts`.
* **Serial log** only over USB (COM6, Espressif USB VID 303A): `idf.py -p COM6 monitor`. Every 60 s it prints the
  N2K summary: frames, dropped, bus state, error counters by type, each PGN / source pair, TIME and POS.
* Ship's Automation watches the box in return: rule WF-004-BOX nudges when the box stops answering on .242 (seen
  10-07 when the box sat on .189 - ANQ-32).

## Failure modes

| Failure | What the box shows / does | Still works | Recovery |
|---|---|---|---|
| **Power lost** (N2K bus / Always On Bus off) | box dark; DO0 / DO1 open (no lamp) | nothing on the box; the panel and the Pi carry on | power back: outputs OFF through boot, clock "--:--" until two agreeing GPS time messages, circle restored from NVS (no age limit, ANC-22) and the watch carries on; a restored ALARM re-asserts DO0 at once. Armed without a circle -> re-armed fresh with the saved rode. **Not fail-safe:** a dead box lights nothing - the Pi's reachability rule is the only alarm for it. Power-cycle test PARKED, risk accepted (ANQ-19) |
| **GPS position lost** while armed (no 129025) | after 30 s with no accepted fix: `GPS STALE` warning, WATCH woken; the status line `gps NONE` | the circle stays frozen; alarm resumes on the next fix | find the MSC-10 / N2K fault. A stale watch cannot alarm - it is blind (the warning is the alarm) |
| **GPS time lost** | the clock keeps running on the box's own timer from the last accepted time | everything | none needed. After a reboot without GPS time the clock is "--:--" (the RTC chip is not used) |
| **Wrong / second GPS on the bus** | n2k.c takes 129025 / 126992 from ANY source; two GPS sources interleaving would look like jumps (filtered, then accepted after 10 in a row) | - | today only the MSC-10 sends position (F-135). If a second source is ever added (e.g. the plotter's GPS, Ship's Automation OQ idea), filter by source first - ASSUMED - check your wiring before adding one |
| **N2K bus silent** (nothing heard 10 s) | `N2K bus silent` notice, no lamp (design, confirmed by 10-21 - ANQ-26); watch goes GPS STALE if armed | screen, Wi-Fi functions | check the drop / bus power. Box CAN errors: ~1,700/s counted in listen-only with every frame decoded and 0 dropped - judged a counting artifact (ANQ-12, parked) |
| **Cerbo silent** 180 s while the bus lives | DO1 -> AL-13 'Automation', `Cerbo silent on N2K` | anchor watch | released 30 s after Cerbo frames return |
| **Pi / SignalK down** (60 s unhealthy, router answers) | DO1 -> AL-13, `Pi / SignalK not answering` | anchor watch, clock | released after 30 s healthy. With the router silent: `Network down - router not answering`, no lamp |
| **Rules engine down / Wi-Fi down** | after 2 failed polls: `Night watch: no link` (bar greyed, not tappable); CLOCK line `Alerts: NO LINK to the rules engine`; ALERT screen leaves | anchor watch, DO0, Cerbo watch, clock | nothing on the box; fix the engine / LAN. The panel and horns do not depend on the box |
| **Engine refuses a tap** | `not changed: token refused (401)` / `ACK not sent: ...` for 30 s | - | token mismatch: the box's `CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN` and the engine's NIGHT_WATCH_TOKEN must be equal; fix, rebuild, OTA |
| **Box on the wrong IP** (DHCP reservation missing) | everything works; `ota_push.py --host .242` and WF-004-BOX miss it | all | add / fix the reservation (AnchorAlarm, MAC XX:XX:XX:XX:XX:XX) on the router, then reboot the box (an OTA push reboots it) |
| **Status page dark, uptime counting** (10-05) | page / OTA unanswered while the box runs on | watch, outputs | fixed in v0.6.0 (socket pool 16, LRU purge, stalled-OTA abort - ANC-30); closed 10-07 (ANQ-29). If it recurs: count client sockets vs `LWIP_MAX_SOCKETS` |
| **Internal RAM sliding** | `lowest` creeping down (23 KB after the first ALERT draw on v0.7.0, 20 on v0.9.0) | - | fixed by ANC-42 from v0.9.1 (ANQ-33 closed 10-11): v0.13.0 102 KB free, lowest 67 after 49.7 h (10-23). If it slides again: big buffers to PSRAM (heap_caps). Allocation failure in the UI or Wi-Fi is the risk |
| **Touch dead** | GT911 not answering at boot -> the box runs WITHOUT touch (log `no answer at 0x5D`) | watch keeps running if it was armed; alarm still drives DO0 and the screen | cannot ARM / DISARM / MUTE from the glass; USB log; reseat / replace the board. (the original firmware had dead touch; ours works - first flash 10-02) |
| **Expander write fails** | log `expander refresh failed` every 5 s; outputs may hold their last level | - | I2C fault; the 5 s rewrite (ANC-16) restores the outputs after a glitch |
| **Update refused** | `REFUSED (409): watch armed or alarm showing` | - | disarm first (with the owner) - by design (ANC-24) |

## OTA, slots and rollback

The box has two app slots. The running slot is shown as `running ota_0|ota_1`; an update always writes the OTHER
slot, then `esp_ota_set_boot_partition` + restart.

1. Upload: `POST /ota` streams the image into the idle slot; `esp_ota_end` checks the image and its RSA signature
   against the running app's key. A bad or unsigned image is refused, nothing changes, no restart (tested 10-02).
2. First boot of the new image: state PENDING_VERIFY. The clock task marks it good (`esp_ota_mark_app_valid_cancel_rollback`)
   after 30 s of running with the display and clock loop up.
3. Any reset before that (crash, watchdog, brown-out) -> the bootloader boots the OLD slot again (rollback). The
   status page then shows the old `running` slot and version.
4. After 30 s the new image is final. **An image that runs 30 s but has broken Wi-Fi / HTTP is kept** - it can
   no longer be updated over Wi-Fi: recovery is USB.

| Situation | Recovery |
|---|---|
| Push timed out / no answer after the restart | read `--status` a minute later; if `running` did not change, the image rolled back or never installed - read the USB log at the boat |
| New image misbehaves but has Wi-Fi | build the previous tag (`git checkout v0.N.0`, release build) and push it - the box takes any image signed with our key, newer or older |
| New image has no Wi-Fi / HTTP (bricked for OTA) | USB at the boat: `idf.py ... -p COM6 flash` of a good build (writes bootloader, table, otadata, app to ota_0; NVS is kept, so the circle / pin / Cerbo table survive) |
| Signing key lost | USB flash with a NEW key (new `keys/anchorbox_signing_key.pem`); from then on only images signed with the new key install |
| Box will not boot at all | USB: `esptool ... erase-flash` then a full flash (NVS lost: re-pin the zone, re-ARM). Back to the original firmware: write the backup image at 0 (Build-Flash-Test) |

Rules that always apply (CLAUDE.md): updates only with the owner's go-ahead; commit + tag before a release build;
after every push read `--status`; never leave a bench / test build on the box; USB flashing at the boat with the owner,
after an `esptool read-flash` backup.

## Gotchas in operation

* The update gate also blocks while the watch is merely ARMED - updates wait until the boat is not at anchor.
* Pi watch is ON in every build from `sdkconfig.defaults`: a build pushed while nothing answers
  192.168.1.10:3000 lights AL-13 about 60-70 s after boot.
* The `router` word on the status page is the last check, which can be hours old - read its age (v0.13.0, ANQ-32).
* The night-watch bar is hidden in daylight unless ARMED or nudging (ANC-31) - "no bar by day" is correct.
* An ack from the box acks EVERY active engine alarm and stops ack-or-kill countdowns (ANC-33, D-122).
