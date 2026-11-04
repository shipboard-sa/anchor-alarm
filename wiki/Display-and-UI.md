<!--
Purpose: What the anchor box shows and when - CLOCK and WATCH views, the buttons, the pages (ARM, DISARM, RE-ARM,
  ZONE, PLOT), the ALARM banner, the rules between views, the anchor symbol and night red.
Version: v12 (2026-10-23) - v12: RESET + WATCH CHECK-IN live since v0.13.0 10-21 20:44 CT (headings were 'built, not on the box'). v11 (2026-10-21) - v11: batch 2 BUILT as v0.13.0 (RESET on ALERT, WATCH CHECK-IN, router age); 2.3 closed by ANC-48. v10 (2026-10-21) - v10: 'Queued, not built' section (Build-Queue batch 2, 10-21). v9.1 (2026-10-18) - v9.1: EXPECTED? YES / NO only (10-18). v9 (2026-10-18) - v9: EXPECTED? screen - the cold-box restock question (ANC-45). v8 (2026-10-15) - v8: DOCK on the ARM page + AT DOCK / LEAVE on WATCH (ANC-44). v7 (2026-10-11) - v7: HOT WATER sixth WATCH button (ANC-43). v6 (2026-10-09) - v6: FILL button on WATCH + FILL page (ANC-41, place by 10-09). v5 (2026-10-07) - v5: tap-to-ack + panel-LED model (ANC-33). v4 (2026-10-07) - v4: ALERT screen + CLOCK alerts line + WATCH engine line (ANQ-10, ANC-32); night-watch bar only after dark (ANC-31). v3 (2026-10-06) - v3: night-watch bar on the CLOCK (ANQ-30, ANC-28); digits moved up. v2: the real WATCH screen as live (v0.4.0): state-driven buttons, RE-ARM, breadcrumb
  PLOT, ZONE pin, ALARM banner. v1: CLOCK built, WATCH placeholder
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page; code in main/screens.c, ui_clock.c, ui_watch.c; data from watch_ctl.c,
  boatclock.c, nightwatch_ctl.c (night watch), alerts_ctl.c and fill_ctl.c (tank fills, ui_fill.c).
Invocation: n/a (reference doc)
-->

# Display and UI

## CLOCK (default / screen saver)

* 24-hour HH:MM in local boat time, **red on black**, seven-segment digits filling the 800 x 480 screen
  (140 x 300 px each, 28 px strokes). Mock-ups: `../docs/mockups/`.
* "--:--" until GPS time is accepted AND a zone is known (ANC-07, ANC-19) - never a guessed time.
* Bottom right: the **anchor symbol** (ANC-06): none = OFF, outline + slow blink = ACQUIRING, solid = LOCKED or
  ALARM, solid + '!' = CONSERVATIVE.
* Bottom left: **'M'** while the time zone is pinned (ZONE page).
* Night (sun below the horizon at the boat): the red drops to dark red (ANC-08, ANC-21). The backlight itself is
  on / off only (Hardware).
* Under the digits: the monitoring-host notice (ANC-25), then the **night-watch bar** (below).
* Layout (v3, 10-06): digits at the top (TOP 14 px, was 70) so the bar fits under them; 'M' bottom left, anchor
  symbol bottom right, the bar between them.

### Night-watch bar (ANQ-30, ANC-27 / ANC-28)

The box sits at the foot of the companionway ladder: tap it to arm night watch going to bed, tap again before going
up on deck. The state lives in the rules engine (Ship's Automation D-118); the bar shows only what the engine reported.

| Bar | Means | Tap |
|---|---|---|
| **Solid red bar**, black 'NIGHT WATCH ARMED' / 'tap to turn off' | engine says armed | asks the engine to turn it off |
| **Hollow outline**, red 'NIGHT WATCH OFF' / 'tap to arm' | engine says off | asks the engine to arm |
| Hollow, **full red (even at night), thick edge, blinking**, 'NIGHT WATCH OFF' / 're-arm?  tap to arm' | engine's re-arm nudge (night, nobody in the cockpit for a while) | arms |
| same shape + 'sending ARM ...' / 'sending OFF ...' | tap on its way | - (wait for the engine's answer) |
| same shape + 'not changed: token refused (401)' (30 s) | the engine refused the tap | try again / fix the token |
| thin dim outline 'Night watch: no link' | engine not answering (2 polls in a row) or no Wi-Fi | - (a tap wakes WATCH) |
| any state + 'no token - can't change it here' | no token built in | - |

**Shown only after dark (10-07, ANC-31):** the bar appears when the box's own night flag is on (sun altitude -
the same flag that dims the red). In daylight it is hidden - and so not tappable - unless night watch is ARMED (a watch
left on in the morning must be seen before going up on deck) or the engine is nudging. Daylight + OFF / no link =
just the clock.

One tap, no confirm page (owner: 'one big toggle'). Not on the WATCH screen, never during an anchor ALARM (ALARM
forces WATCH). Visual only: the box has no buzzer. Status page line: `night watch ARMED (by ..., since ...) - link
ok, last answer N s ago, token set` or `night watch: NO LINK - engine ..., last answer ..., token ...`.

## WATCH

Top: the state in large text ("WATCH OFF", "ACQUIRING 3:12", "LOCKED", "CONSERVATIVE", "ALARM") and the clock with its
zone ("18:07 CT (auto)"). Then: distance from the anchor / centre and the alarm distance; the rode and why it has
not locked yet; warnings (DRAG?, GPS STALE, NO GPS POSITION); the last three events.

**The bottom buttons follow the state** (10-02; FILL added as the fifth, 10-09 - ANC-41; HOT WATER as the
sixth, 10-11 - ANC-43):

| State | Buttons |
|---|---|
| OFF | ARM - PLOT - ZONE - CLOCK - FILL - HOT WATER |
| OFF + the engine says DOCK (ANC-44) | LEAVE - PLOT - ZONE - CLOCK - FILL - HOT WATER |
| Armed (ACQUIRING / LOCKED / CONSERVATIVE) | DISARM - RE-ARM - PLOT - CLOCK - FILL - HOT WATER |
| ALARM | DISARM - MUTE - RE-ARM - PLOT - (empty) - (empty): no FILL / HOT WATER in an ALARM |

FILL reads **FILLING** while a fill session is on (tap or watermaker, once its string rises); dark on a build without
Wi-Fi. The six buttons are 120 px wide (143 in v0.9, 180 before); a label is 28 pt when it fits, else 20 pt
(DISARM, FILLING). Six is the most one row takes: the next button means a second row (10-11: 'at some point,
you can make a second row of buttons if we need more UI space').

**HOT WATER** (ANC-43, Ship's Automation D-135) is a two-line button: one tap asks the rules engine for a one-off
water-heater run (POST /hot-water, no confirm page); a second tap stops it. The ENGINE switches the heater and ends
the run (tank hot / no draw in 2 min / 20 min) and refuses below 50 % house SoC. Faces: `HOT WATER`, `HOT 14 min`
(heavier border, counted down between polls), `HOT held` (the boat holds the heater off - high-water lockout or
inverter shed), `HOT READY` for 30 min after a run that ended hot, `HOT ...` while a tap is on its way; dark without
Wi-Fi or the box token. The notice line above the buttons (below an engine alert and the monitoring-host notice)
says `HOT WATER heating - 14 min left, ends when the tank is hot`, the engine's refusal for 30 s (`house bank 46 %
- below the 50 % floor for the heater; use the kettle`) or, for 10 min, how the run ended.

**DOCK / LEAVE** (ANC-44, Ship's Automation D-151 - 10-15). The ARM page's bottom row is `don't know` - `DOCK /
slip fence` - `CANCEL` (248 px each): DOCK is the ENGINE's boat-mode tap (POST /mode DOCK) that arms the WF-108 slip
fence, not this box's watch; dark when it may not send (no link / token, already DOCK, or over an armed ANCHOR with
no crew phone aboard). While the engine says DOCK (whoever tapped it) and the box watch is OFF, WATCH's two lines read
`AT DOCK - slip fence armed` / `LEAVE ends it (boat mode -> MOTOR)` - or `LEAVE needs one of our phones aboard - or
end it from the phone / tablet` - and LEAVE takes ARM's slot. LEAVE opens a confirm page (`LEAVE the dock?` - YES -
LEAVE / CANCEL) and is dark unless the engine reports a crew phone on the boat Wi-Fi (`leave_ok`); the page closes by
itself if that changes while it is open. A refusal (403 - its reason) or error shows on the second line for 30 s.
No link = no AT DOCK, the normal OFF lines and ARM.

Pages (full screen, CANCEL / BACK on each):

* **ARM** - rode out: 50, 100 ... 300 ft, **ALL 328 ft** (100 m of chain aboard), or "don't know" (no coarse
  guard). Arm after the set (backed down, neutral) - SPEC 5.3.
* **DISARM** - "YES - DISARM" to confirm. Releases DO0, drops the circle.
* **RE-ARM** - the owner's "reset the circle" (SPEC 5.13): drops the old circle and starts a fresh watch from here with the
  same rode; the page reminds him to check the breadcrumbs first. Always his call.
* **MUTE** (ALARM only) - stops the banner flashing. DO0 stays on until DISARM (ANC-23); the panel has its own MUTE.
* **ZONE** - AUTO, US Central, US Eastern, Bahamas, Atlantic, UTC-4 / -5 / -6. Kept across power-ups. Offered
  while the watch is OFF.
* **PLOT** - the breadcrumb trail: north up, east right, the alarm circle as a ring, the anchor / centre as '+',
  the boat now as a bright dot, the last 6 h at one point per 10 s (recorded armed or not). Scales itself so the
  circle and the whole trail fit (never tighter than +-30 m); the plot edge in metres is shown beside it.

**ALARM:** a red banner over the top of the screen, black text "ANCHOR ALARM / distance - circle", flashing at
1 Hz until MUTE, then steady. The banner steps aside while a page (PLOT, RE-ARM, DISARM ...) is open - that is
where the decision is made.

Disabled buttons are drawn dark (never the theme's bright grey) for night vision.

## Rules (screens.c)

| Event | Result |
|---|---|
| Tap anywhere on CLOCK | WATCH. The tap is swallowed (switch on press + wait for release) |
| Tap on the night-watch bar (when tappable) | asks the engine to flip night watch; stays on CLOCK |
| 2 min with no touch on WATCH | back to CLOCK (not during ALARM) |
| ALARM | full brightness, WATCH, held - no timeout (ANC-05) |
| DRAG? or GPS-stale warning | wakes WATCH (not an alarm) |
| Unacked engine P1 / P2 (ANQ-10) | ALERT screen - from CLOCK at once, from WATCH left alone 15 s; **never during an anchor ALARM** (the anchor ALARM also takes the screen away from ALERT) |
| Tap anywhere on ALERT, something unacked | **ACK** (POST /ack as `anchor-box`); frame goes steady; stays on ALERT (ANC-33) |
| Tap anywhere on ALERT, all acked | CLOCK (night-watch bar), held 2 min after the last touch, then ALERT again while anything is active |
| Every alert CLEARED, or link lost | ALERT goes back to CLOCK (acked-but-active ones stay - panel-LED model) |
| Tap on the CLOCK alerts line | ALERT at once |
| FILL on WATCH | FILL page (never during an anchor ALARM) |
| BACK on FILL | WATCH |
| 2 min with no touch on FILL | CLOCK |
| Anchor ALARM, DRAG? or GPS-stale while on FILL | WATCH at once |
| Unacked engine P1 / P2 while on FILL | ALERT once nobody touched it for 15 s (as from WATCH) |
| A cold box asking 'expected?' (ANC-45) | EXPECTED? screen - from CLOCK at once, from WATCH / FILL left alone 15 s; never during an anchor ALARM; an engine ALERT goes first |
| YES / NO on EXPECTED? | one tap answers (POST /restock), no confirm; `YES taken ...` / `NO taken ...` 5 s, then CLOCK |
| Press off the buttons on EXPECTED? | nothing - YES / NO only (10-18); the night-watch bar waits until it is answered |
| Engine ALERT while on EXPECTED? | ALERT (unacked at once, acked-only after 2 min untouched - as over the CLOCK) |
| Nothing asking any more, or link lost | EXPECTED? goes back to CLOCK |

## ALERT (ANQ-10, ANC-32 / ANC-33) - the rules engine's alarms; tap anywhere to ACK

The engine's active P1 / P2 alarms (`GET http://192.168.1.10:8200/alerts`, every 2 s; contract in Ship's Automation
`spec/04-Interfaces/anchor-box-alerts.md`). **Like the panel LEDs (10-07, ANC-33): flashing frame = unacked,
steady = acked but still active; everything stays on the screen until it CLEARS. A tap anywhere ACKS** (same effect as
the ACK / MUTE button S2-DI6: silences the Pi horns once they are wired, stops ack-or-kill countdowns, ends a test -
risk accepted by the owner). Black screen, red text at full brightness even at night, 12 px red frame (a coloured screen
stays the anchor ALARM's, ANC-06). Head: `P1  WF-201  2 min ago` (+ `TEST`
for a drill, `(acked)`); the top alert's WHOLE message (up to 340 chars, 8 lines); up to three more one line each, or
`+ N more - see the cockpit`; footer `TAP ANYWHERE TO ACK` (large) / `sending ACK ...` / `ACK not sent: <why>` (30 s) / `All acked - shown until it
clears.   Tap: clock`. NUDGEs never
show here (phones + cockpit); AIS alarms on the box are queued (ANQ-37, below).

| Where | Shows |
|---|---|
| CLOCK, under the notice | `2 ALERTS - 1 not acked - tap here` / `2 alerts (acked) - tap here`; **`Alerts: NO LINK to the rules engine`** when the engine stops answering (2 polls) - never blank, silence must not read as all-clear |
| WATCH notice line | `ENGINE P1 WF-201: ...` while one is unacked - outranks the AUTOMATION notice, and stays visible under the anchor ALARM banner (banner 318 px high) |
| Status page | `alerts seq N: T active, U not acked[, HORNS], top WF-nnn - link ok, last answer N s ago \| ack token set[, last tap ACK ok / FAILED]` |

## FILL (ANC-41, Ship's Automation D-129) - tank-fill taps

A fill is STARTED by the crew (10-09: the senders slosh 5-6 points, so the engine no longer guesses fills); the
rules engine then gives the ETA and the 90 % / 95 % (full) chirps and ends the session itself (20 min with no rise,
4 h, or the watermaker stopping). The page shows only what the engine reported (`GET /fill`, every 2 s while open).

Head: `TANK FILLS` and, right, `Watermaker RUNNING 820 W` / `Watermaker off`. Then one row per tank the engine lists
(port FW, starboard FW, port fuel, starboard fuel), each framed (heavier frame while filling):

| Row line 2 | Button | Tap |
|---|---|---|
| `85 %   not filling` | **FILLING** | starts a session (POST /fill on) |
| `92 %   FILLING - started by api:anchor-box` | **STOP** | ends it (POST /fill off) |
| `40 %   FILLING from the watermaker` / `watermaker running - not rising here` | `watermaker` (dark) | none - ends with the watermaker (D-130 cut depends on it) |
| any + `sending ...` on the button | - | wait for the engine |

`level ?` = sender UNKNOWN. A label longer than 32 characters loses its trailing `(...)` ('STARBOARD FRESH WATER').
Footer: the hint (`FILLING: chirps at 90 % and full (95 %) - ends by itself`), `sending FILLING ...`, a refused tap for
30 s (`not changed: token refused (401)` ...), `no token - can't change it here (cockpit page)`, or `No link: ...`
(`the rules engine has no fill sessions yet (404)` before engine api.py v9 is deployed). No link = no rows (never a
stale level). BACK bottom right. One tap, no confirm page (a session switches nothing). Red on black like WATCH.

## EXPECTED? (ANC-45, Ship's Automation WF-502 / D-128) - the cold-box restock question

Black, red text, a thin steady frame (a question, not an alarm). Head `EXPECTED?`; the question in 28 pt:
`AFT FREEZER warm 10 min - warm food just went in?`; two big buttons (340 x 150): `YES` / `give it 2 h` (from the
engine's hold_h) and `NO` / `alarm now`. One box at a time (fridge, Engel, aft, forward). Footer: `one tap answers -
no answer: the alarm comes at 30 min as before`, `... 1 more box asking after this ...`, `sending YES for the AFT
FREEZER ...`, the engine's refusal for 30 s (409: the alarm is already up), or `no token on this box - answer on the
cockpit page` (buttons dark). Not answering changes nothing: the engine's WF-501 alarm comes at 30 min as before,
and then the ALERT screen shows it.

## RESET on ALERT (ANC-46, Ship's Automation D-163) - LIVE v0.13.0 (10-21); proof aboard = ANQ-40

A RESET button (bottom right, 220 x 70) shows on ALERT for the first alert the engine marks `rst: true`. It opens a
confirm page: 'RESET <rule>?' + 'The Pi checked: its input reads normal again. RESET clears this alarm and releases
what it switched off. Only if you have looked at the cause.' + RESET / CANCEL (340 x 120 each). RESET posts
/reset/<rule> with the box token; the footer shows 'sending RESET ...', then 'RESET <rule> done' or the engine's reason
(30 s). The page closes itself when the alarm is no longer resettable, after 60 s, or when ALERT goes away.

## WATCH CHECK-IN (ANC-47, Ship's Automation WF-614 / D-161) - LIVE v0.13.0 (10-21); proof underway = ANQ-40

Full brightness, black background, red text, thick frame (steady at stage 1, blinking from stage 2). Head 'WATCH
CHECK-IN' / '... - SECOND CALL' / 'WATCHKEEPER NOT CHECKING IN'; body 'N min since the last I'm up. At 25 min the
alarm wakes everyone.'; footer 'TAP ANYWHERE: I'M UP'. A tap anywhere sends I'M UP; 'I'M UP taken - next check-in in
15 min' shows 4 s, then the CLOCK. Order of precedence: anchor ALARM > engine ALERT > WATCH CHECK-IN > EXPECTED?.

## Status page (v0.13.0)

New line `watchkeeper: off (DOCK) - link ok ...` / `ON stage N ...`. The host-watch line now gives the router check's
age: `router SILENT 3 h 05 min ago` (ANQ-32, seen live 10-23: `router answered 9 h 17 min ago`) - the router is only checked while the Pi looks unhealthy.

## What the box does NOT show (ANC-48, 10-21)

Only immediate-action events: the anchor ALARM, engine P1 / P2 (ACK / RESET), the watch check-in, one-tap questions.
NUDGEs - the A-100's own AIS alarms (WF-605-ALR) included - live in a separate 'what is active' view, not here
(Build-Queue 2.3 closed this way).

## Choices behind the look

Black background day and night; a coloured area only ever means ALARM (ANC-06) - except the solid night-watch bar
(ANC-28), which is steady, bar-sized and only on CLOCK, never shown with an ALARM. Red keeps night vision. Pick
lists, never typing. Every destructive action (DISARM, RE-ARM) is confirmed on its own page.
