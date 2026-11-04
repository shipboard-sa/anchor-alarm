<!--
Purpose: Functional spec for the NEW clean-room anchor box firmware (ESP32-S3 Waveshare 4.3B) - what it does,
  its screens, its outputs, and the open open questions.
Version: v30 (2026-10-21) - v30: NEW 16 crew RESET, 17 watch check-in; 11.5 = immediate-action events only (10-21, ANC-48). v29.1 (2026-10-18) - v29.1: 15.7 YES / NO only, no 'not now' (10-18). v29 (2026-10-18) - v29: NEW 15 cold-box EXPECTED? question (10-18, Ship's Automation WF-502 / D-128, ANC-45). v28 (2026-10-15) - v28: NEW 14 DOCK / LEAVE taps (10-15, Ship's Automation D-151, ANC-44). v27 (2026-10-11) - v27: NEW 13 HOT WATER tap (10-11, Ship's Automation D-135, ANC-43). v26 (2026-10-09) - v26: NEW 12 tank-fill taps (10-09, Ship's Automation D-129, ANC-41). v25.1 (2026-10-07) - v25.1: 7.3 names the ack-or-kill exception (ANC-33 / D-122). v25 (2026-10-07) - v25: 1.3 / 1.5 / 7.2 / 9.3 / 9.7 brought up to the live system (v0.8.0); NEW 10 night-watch remote, NEW 11 engine alerts (rebuild-from-spec pass). v24 (2026-10-03) - v24: NEW 9.x monitoring-host watch on DO1 -> AL-13 (10-03, Ship's Automation OQ-117). v23: LIVE (v0.4.0) - 1.3 / 1.5 as built (DO0, listen-only, Wi-Fi for updates only), answered questions closed (6.2, 6.4, 8.6, section 4). v22: 7.5 plotter alert test facts carried over from boat-automation (F-171, OQ-114). v21: 7.4 watch N2K general messages + alarms; the chartplotters are not handling them well (owner). v20: tolerance 15 m fixed, tune after shakedown (5.14); the owner's alarm-response workflow (5.13). v19: 100 m (328 ft) of chain aboard (5.1); re-arm after power cut ACCEPTED (5.12). v18: NEW 8.x firmware updates over Wi-Fi (10-02, to drop the USB cable). v17: NEW requirement 7.1 (10-02, at the boat): the touchscreen as the boat's always-on alarm / alert / acknowledge screen - scope open (ANQ-10); Pensacola zone corrected to US Central (3.4). v16: project split out of Ship's Automation (ANC-14) - ids now ANC-nn / ANQ-nn. v15: hardware findings from the Waveshare schematic (6), step 2 built. v14: 5.4 rode floor = 2 x reach (geometry), step 1 built in anchor-watch. v13: conditions split decided (5.11). v12: conditions idea recorded. v11: v10's rode floor on LOCKED circles REVERTED - circle = observed geometry + tolerance. v10: whole circle is safe. v9: MSC-10 ~60 ft from the bow. v8: bow-to-antenna ~50 ft. v7: coarse guard while acquiring + rest of the anchoring workflow. v6: rode floor + the anchoring workflow (section 5). v5: WATCH -> CLOCK after 2 min. v4: time zone automatic from position + manual override. v3: armed indicator = red anchor symbol, black background. v2: the owner's answers on the clock, alarm override, indicator, dimming, boat time
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: . (separate project, private GitHub repo, ANC-14) - requirements contract. Algorithm = C port of
  the reference model in reference/ (circle fit). Decisions ANC-nn in wiki/Design-Decisions.md, open items
  ANQ-nn in CHECKPOINT.md. Nothing from the original proprietary firmware is used (proprietary, ANC-01).
Invocation: n/a (reference doc)
-->

# Anchor box - firmware spec (clean-room build)

Status markers: **LEE** = Owner stated it (date). **DESIGN** = Claude's technical choice. **ASSUMED - check your wiring** =
not settled; nothing gets built on it until answered.

## 1. What the box is

| # | Item | Source |
|---|---|---|
| 1.1 | Hardware stays as it is: the box aboard (Waveshare ESP32-S3 4.3B, 800x480 touch LCD). New firmware only | OWNER 10-02 |
| 1.2 | Clean room: no code, structure or text from the original proprietary firmware. Hardware facts come from Waveshare / Espressif / chip datasheets and permissively licensed components | OWNER 10-02 (ANC-01) |
| 1.3 | Independent of the Pi. GPS + time from the boat's NMEA 2000, listen-only (the box never transmits on the bus - a transmitting alert test is parked, ANQ-17). Wi-Fi carries signed updates, the read-only status page (section 8), the monitoring-host watch's SignalK poll (section 9), the night-watch remote (section 10) and the engine alerts (section 11); the ANCHOR watch never depends on it and nothing on the network can arm / disarm it | OQ-098; DESIGN; LEE 10-02 (Wi-Fi updates); ANC-27 |
| 1.4 | Anchor logic = the circle fit (lock rule, frozen centre, conservative fallback, 20 s debounce), ported from reference/ (was Ship's Automation stacks/anchor-watch) with the same defaults and the same test cases | OQ-098 (the original design 09-30) |
| 1.5 | Outputs: **DO0** (as wired, with GND) -> panel Anchor Drag input (AL-12, Pi tap S2-DI4) asserted for the whole ALARM incl. MUTED, released on DISARM / ARM / RE-ARM; no buzzer fitted; DO1 = monitoring host down -> panel AL-13 (section 9, wired + proven 10-05); all outputs OFF from power-up through boot. Proven 10-02 (AL-12 lit) | CAMS 1.5 spec; LEE 10-02 (wiring, ANC-17) |

## 2. Screens

| # | Item | Source |
|---|---|---|
| 2.1 | Two views: **CLOCK** (idle / screen saver, the default view) and **WATCH** (the anchor watch GUI) | OWNER 10-02 |
| 2.2 | CLOCK is a 24-hour clock in **local boat time**, using the **whole display** as the clock face: **large digital HH:MM, red text** (on black - DESIGN: keeps night vision) | OWNER 10-02 |
| 2.3 | A tap anywhere on CLOCK wakes the box into WATCH. The wake tap is used only to wake; it never presses a WATCH control | OWNER 10-02 (tap wakes); DESIGN (tap swallowed) |
| 2.4 | WATCH falls back to CLOCK after **2 minutes** with no touches (never while ALARM is showing) | OWNER 10-02 |
| 2.5 | An ALARM **overrides the screen**: full brightness, WATCH at once, held there until acknowledged. GPS lost while armed does the same | OWNER 10-02 (alarm overrides); DESIGN (GPS lost treated as an alarm) |
| 2.6 | CLOCK shows the watch state as a **small red anchor symbol on the black background** (no background colour change, day or night): OFF = no symbol; ACQUIRING = outline anchor, slow blink; LOCKED = solid anchor; CONSERVATIVE = solid anchor + small mark. A coloured full screen is reserved for ALARM | OWNER 10-02 (symbol only, black background); DESIGN (shape per state) |
| 2.7 | Night: dim the backlight on CLOCK. Night = sun below the horizon at the boat's position, worked out on the box | OWNER 10-02 (dim at night); DESIGN (sunset from position) |

## 3. Time

| # | Item | Source |
|---|---|---|
| 3.1 | Time source = UTC from the GPS on N2K (system time / GNSS position data); the ESP32 keeps time between fixes | DESIGN |
| 3.2 | **Local boat time = UTC adjusted by the local time zone, picked automatically from the boat's position, with a manual override** | OWNER 10-02 |
| 3.4 | Zone table built in for the cruising grounds: US Central (NOLA, Pensacola - the Florida panhandle west of the Bay / Gulf county line, ~85.39 W, between Mexico Beach and Port St. Joe; corrected 10-02, was wrongly 'Pensacola = Eastern') and US Eastern (Port St. Joe eastward, Tampa) with US daylight saving; Bahamas = Eastern with daylight saving; Puerto Rico, Lesser Antilles, ABC islands = Atlantic, no daylight saving. Offshore / outside the table = nautical zone (longitude / 15, rounded). Table grows as the itinerary does | DESIGN |
| 3.5 | Manual override on WATCH: pin a fixed zone (e.g. keep the departure port's time on passage) or return to AUTO. The clock shows a small 'M' while pinned so an override is never forgotten | OWNER 10-02 (override); DESIGN ('M' mark) |
| 3.3 | Before the first GPS time after power-up: the clock shows "--:--", never a wrong time | DESIGN |

## 5. Anchor logic - boat specifics

| # | Item | Source |
|---|---|---|
| 5.1 | **No chain counter aboard.** Rode out is an estimate, at best to the nearest 50 ft. **100 m (328 ft) of chain aboard, all chain** - the pick list is 50..300 ft + ALL (328 ft) + don't know | OWNER 10-02 |
| 5.2 | **Anchoring workflow:** drop, then back down on the rode to stretch it out and set the anchor. The anchor often MOVES while setting, so its final position is not known until it is set. The back-down shows quickly whether it holds; if not, pick up and restart. Crew watches closely through the early part. If conditions permit, the skipper free-dives the rode / anchor to check it | OWNER 10-02 |
| 5.3 | Therefore: no "anchor down" tap (the drop point is not the anchor's final position). Arm the watch AFTER the anchor is set (backed down, engine in neutral). The lock rule's "centre stable over the last 3 min" already refuses to lock while the anchor is still moving | OWNER 10-02 (no tap, follows from 5.2); DESIGN (arm after set) |
| 5.4 | **Rode floor:** at arming, pick the rode out from a pick list in 50 ft steps (50, 100, 150 ... ft). reach = rode + bow-to-antenna. The CONSERVATIVE (no-lock) circle is never smaller than **2 x reach** + tolerance, centred on the centroid of the fixes seen: the anchor is within reach of every fix (so of their centroid) and the boat may later lie anywhere within reach of the anchor - so 2 x reach, not 1 x, is what actually fixes the calm-then-180-degree false alarm. Built + tested in reference/ (10-02) | OWNER 10-02 (rode floor OK); DESIGN (2 x, from the geometry) |
| 5.6 | **Coarse guard while ACQUIRING** (first 10-15 min, before a circle locks): ALARM if the boat gets more than 2 x (rode + bow-to-antenna) + tolerance from the arming position. Arming is ~one rode from the anchor, and the furthest legit swing is the far side, so a normal swing never trips it. Ends when the circle locks or the conservative circle takes over | OWNER 10-02 (add it); DESIGN (formula) |
| 5.7 | Pick up and restart = DISARM, then a fresh ARM after the new set; a re-arm discards the old fit | DESIGN (matches anchor-watch) |
| 5.8 | **Bow-to-GPS-antenna: the MSC-10 (the box's position source) is at the very back, ~60 ft (18 m) from the bow.** The other external GPS antennas are at the back of the fixed solar array, ~50 ft, all on the centreline (F-135). Use 60 ft in the rode floor (5.4) and the coarse guard (5.6) | OWNER 10-02 |
| 5.9 | Minimum fitted radius stays 15 m: the antenna can never be closer to the anchor than ~60 ft (18 m), even with the rode straight down, so 15 m sits safely below a physical floor (room for GPS error) | DESIGN (from 5.8) |
| 5.12 | **A power cut never resets the watch.** Once a circle exists (LOCKED / CONSERVATIVE / ALARM) it is saved and comes back unchanged at power-up - **no age limit**: re-fitting after an outage would quietly accept a slow drift. A drag during the outage alarms after the usual debounce; a latched ALARM stays latched. Only DISARM or a fresh ARM drops it (worst case = a false alarm, never a missed one). Armed but no circle yet -> re-armed fresh | OWNER 10-02 (ANC-22) |
| 5.13 | **The owner's alarm response (night, from experience with small drags):** on an alarm he looks at the **breadcrumb trail on the chart plotter** - 'the real interesting diagnosis tool, it's quite clear that we are dragging' - and decides whether to reset. If the risk is low (dragging AWAY from land, no boats behind) he generally just resets the circle. **His decision at the time** - the box never decides that for him | OWNER 10-02 |
| 5.14 | **Tolerance = 15 m, fixed** (alarm circle = observed swing + 15 m; conservative / coarse guard = 2 x reach + 15 m). Tune after the shakedown on real swings (ANQ-04) | OWNER 10-02 (value was DESIGN) |
| 5.10 | **The boat is safe ANYWHERE inside the circle.** Tide change, wind reversal or a slackening wind move the boat relative to the anchor, but the rode still limits her distance from it. Only the OUTER boundary alarms; there is no inner boundary and no 'off the arc' check (already true in anchor-watch: alarm only when distance > alarm radius) | OWNER 10-02 |
| 5.11 | **Conditions - DECIDED split (10-02):** (a) the hard ALARM stays the observed circle + tolerance; conditions NEVER widen it (strong wind is when anchors drag). (b) NEW early **DRAG? warning** from the heuristic 'a wind stretch stops, a drag keeps going': distance from the frozen centre still rising after several minutes without levelling off -> warn, even inside the circle. GPS only. (c) Water speed (DST810 paddlewheel, navigation.speedThroughWater) and wind (gWind via GND 10) are CONTEXT: shown on WATCH, in every warning / alarm, and logged; they change nothing until shakedown data proves how they behave on this boat. The physics: held in current = water speed > 0; dragging with the water = ~0 (caveats: slack water reads ~0 too; a wind drag moves through the water; DST810 direction sensing / low-speed cut-off undocumented - verify) | OWNER 10-02 |
| 5.5 | **The circle comes from OBSERVED geometry plus a tolerance, not from the rode:** LOCKED alarm radius = max(fitted radius, furthest seen) + tolerance. The rode cannot set the circle - it ignores water depth and the roller's height above the water, and on this boat it is a 50 ft guess anyway. The rode pick only floors the CONSERVATIVE (no-lock) fallback, where there is no observed geometry to use (there it is a generous upper bound - its real reach is shorter). Known limit to check on real swings: a fit made while the rode is slack (calm) - expected to fail the arc / RMS / curvature tests and fall to CONSERVATIVE rather than lock small | OWNER 10-02 (observed geometry + tolerance; supersedes v10 5.5) |

## 6. Hardware findings (step 2, 10-02 - Waveshare wiki + schematic + chip datasheets; README.md lists sources)

| # | Finding | Effect |
|---|---|---|
| 6.1 | **The backlight is on / off only in practice.** It is enabled through the I2C expander (EXIO2 -> AP3032 CTRL). The driver accepts PWM, but toggling it over I2C fast enough without flicker is not practical | Night dimming (2.7) = dim the RED of the digits (pixel level) with the backlight on. On an IPS panel the black still glows faintly. A soft-PWM test over I2C can be tried on the bench |
| 6.2 | **The board has no buzzer.** | None fitted, none needed: the panel sounds (ANSWERED, 10-02) |
| 6.3 | Outputs DO0 / DO1 = expander open-drain pins through PC817 optocouplers; the expander powers up with them high = OFF | Matches 1.5 'OFF from power-up'. Mapping + polarity confirmed with a meter at step 5 |
| 6.4 | The expander drives all 8 of its IO pins together, including the two digital-input (DI0 / DI1) terminals | Firmware does not read the DIs; nothing is wired to them (ANSWERED, 10-02) |
| 6.5 | On-board real-time clock (PCF85063A) with a coin-cell socket | Could keep time across power cuts; 3.3 still shows "--:--" until GPS time is seen |
| 6.6 | NMEA 2000 (CAN) on GPIO 15 / 16 through a TJA1051 transceiver; a 120-ohm termination switch (default off) | Step 3 listens only |

## 7. Boat alarm / alert screen (NEW - 10-02; scope answered 10-07, section 11)

| # | Item | Source |
|---|---|---|
| 7.1 | "Can the touchscreen double as our always-on alarm / alert / acknowledge screen?" | OWNER 10-02 (asked at the boat) |
| 7.2 | ANSWERED 10-07 (ANQ-10): the rules engine's P1 / P2 alarms first, with ack from the box (section 11); raw N2K 126983 / 126985 (7.4 / 7.5) later | OWNER 10-07 (ANC-32 / ANC-33) |
| 7.4 | The box should also **watch for N2K general messages and alarms** - 'we found the chartplotters are not playing nice so far' (on the bus today: PGN 126983/126985 alerts from the Cerbo's Alarm Manager src 231 and the Reactor 40 CCU src 1 - Ship's Automation F-111 (spec/03-Findings.md:161), WF-006). What 'not playing nice' means = ASSUMED - check your wiring | OWNER 10-02 |
| 7.5 | **Plotter alert test (Ship's Automation F-171, 10-02):** a valid 126983 Alert (Warning, Technical, system 1 id 1, state active, trigger 'test', acknowledge + temporary-silence supported) + 126985 Alert Text was sent every 2 s for 90 s under the **YDEN-02 gateway's identity** (address 179, its NAME); the gateway echoed it, SignalK decoded it - the **GPSMAP 942xs showed nothing and sent no 126984 response**. The 942xs install guide p.6: receives 126983 / 126985 / 126987 / 126988, transmits 126984 (Alert Response). Theory: the sender must be a **real node with its own address claim, listing 126983 / 126985 in its PGN list**. Frame builder: Ship's Automation stacks/bench/n2k_alert_test.py. The preferred retest = **this box** (Ship's Automation OQ-114): if it works, anchor-drag could alert natively on the plotters (and a plotter ACK could come back as 126984) | OWNER 10-02 (facts from the Ship's Automation chat) |
| 7.3 | Constraints already settled elsewhere: the Master Alarm Panel and helm switches always win (Ship's Automation N-01); panel MUTE stays physical; an ack silences horns, never the panel, and never stops an automatic safety action (Ship's Automation H-03, SO-4) - with the one designed exception that an ack cancels an ack-or-kill countdown (`after_unacked`: the action exists to fire only if nobody answers), which an ack from the box now also does (ANC-33 / D-122, risk accepted by 10-07); CAMS 2.0 plans a 12.3" display in the panel cutout (your-pi-hostname F-104 note) | your-pi-hostname spec |

## 8. Firmware updates over Wi-Fi (NEW - 10-02)

| # | Item | Source |
|---|---|---|
| 8.1 | Update the box over Wi-Fi (OTA) so the laptop USB cable to the box can go | OWNER 10-02 (at the boat) |
| 8.2 | Only images signed with the owner's key install (signature checked on the box) - the previous OTA or anything else on the LAN cannot replace it, whatever address the box has | DESIGN (10-02: 'so the previous OTA update doesn't clobber us') |
| 8.3 | Two app slots + automatic rollback if a new image does not come up healthy | DESIGN |
| 8.4 | No update while the watch is armed or an ALARM is showing; outputs OFF throughout | DESIGN |
| 8.5 | Pushed from the laptop by a script; Wi-Fi password kept in a gitignored file the user fills in (never in git, never typed by Claude) | DESIGN |
| 8.6 | Wi-Fi network: 'YourBoatWiFi' (2.4 GHz side; the box is 192.168.1.100, DHCP-reserved) | OWNER 10-02 |

## 9. Monitoring-host watch - DO1 -> panel AL-13 'Automation' (NEW - 10-03, Ship's Automation OQ-117 1b + 1c)

| # | Requirement | Source |
|---|---|---|
| 9.1 | Use the spare output DO1 as a 'monitoring host down' alarm for the Pi and the Cerbo; replaces the Finder timer relay deferred for cost | OWNER 10-03 |
| 9.2 | The anchor watch stays the first job: DO0 -> AL-12 unchanged; nothing new may block or delay the watch loop; N2K stays listen-only (ANC-03); inform only | OWNER 10-03 |
| 9.3 | DO1 asserted = panel AL-13 'Automation' (DO1 + GND wired to AL-13, proven 10-05) | OWNER 10-03 |
| 9.4 | The screen names WHICH failed: 'Pi / SignalK not answering' or 'Cerbo silent on N2K' | OWNER 10-03 |
| 9.5 | Assert after the threshold; release on its own once the device has been healthy ~30 s | OWNER 10-03 |
| 9.6 | Pi: every 10 s the SignalK health path (value true + timestamp fresh); 404 -> fall back to position freshness; paths + thresholds are config values; unhealthy 60 s -> router check: answers = 'Pi down' + DO1, silent = 'network down', no lamp | OWNER 10-03 (freshness 90 s for the health path - DESIGN, the engine keep-alive is 60 s) |
| 9.7 | Pi side ON since v0.6.0 (10-06, our Pi serves SignalK on :3000); was OFF until the cut-over (ANC-26) | OWNER 10-03 / 10-06 (ANC-29, supersedes ANC-26) |
| 9.8 | Cerbo: alive if ANY frame from one of its N2K addresses within 180 s (incl. its virtual tank devices); addresses from N2K product info / address claim, not hard-coded; silent 180 s -> DO1 'Cerbo silent'; no MQTT, no Modbus, nothing sent to the Cerbo | OWNER 10-03 ('just watching the cerbo generated pgns') |
| 9.9 | Whole N2K bus silent (nothing from anyone 10 s) = not the Cerbo: 'N2K bus silent', no lamp | DESIGN 10-03 - to be confirmed |
| 9.10 | Tests: bench / host replay of Pi down, router down, Cerbo silent, recovery; aboard with owner's yes and the panel muted, by the test build - unplug nothing live | OWNER 10-03 |

## 10. Night-watch remote (NEW - 10-06 / 10-07; Ship's Automation D-118, WF-114)

| # | Requirement | Source |
|---|---|---|
| 10.1 | The box is where night watch is armed going to bed and turned off before going on deck (it sits at the foot of the companionway ladder); one big toggle, no confirm page | OWNER 10-06 (D-118); ANC-28 |
| 10.2 | The state lives in the rules engine; the box shows only what the engine reported, never its own guess; a refused tap says why | DESIGN (ANC-27) |
| 10.3 | Shown only after dark, unless armed or the engine is nudging a re-arm | OWNER 10-07 (ANC-31) |
| 10.4 | Never on WATCH, never during an anchor ALARM; must never delay the anchor watch | OWNER 10-06 / DESIGN (ANC-28) |

## 11. Engine alerts on the box (NEW - 10-05 / 10-07; Ship's Automation anchor-box-alerts.md, D-122)

| # | Requirement | Source |
|---|---|---|
| 11.1 | The box shows the alarm TEXT of the rules engine's active P1 / P2 alarms - the panel has no lamps left and the box is where the crew looks in every alarm response | OWNER 10-05 / 10-07 |
| 11.2 | Like the panel LEDs: flashing = unacked, steady = acked; an alarm stays on the screen until it CLEARS | OWNER 10-07 (ANC-33) |
| 11.3 | A tap anywhere ACKS (same effect as the S2-DI6 button, incl. stopping ack-or-kill countdowns and ending a test) - RISK ACCEPTED | OWNER 10-07 (ANC-33, D-122) |
| 11.4 | The anchor ALARM always wins the screen; silence must never read as all-clear (link lost -> NO LINK line) | DESIGN (ANC-32, D-088) |
| 11.5 | NUDGEs are not shown on the box - the box is for immediate-action events; 'what is active' lives in a separate view | OWNER 10-21 (ANC-48) |

## 12. Tank-fill taps (NEW - 10-09; Ship's Automation D-129)

| # | Requirement | Source |
|---|---|---|
| 12.1 | A tank fill is STARTED by the crew with a 'Filling' tap - from the anchor box most of the time (or the cockpit page); the engine then gives ETA + 90 % / 95 % (full) chirps and ends the session itself | OWNER 10-09 (D-129) |
| 12.2 | The box shows each tank's level and whether a fill session is on (and whether the watermaker is running), and taps FILLING / STOP per tank | OWNER 10-09 (task) / DESIGN (ANC-41) |
| 12.3 | Reached from a FILL button on the screen a tap on the CLOCK brings up (WATCH), beside its other buttons - daytime is when fills happen and the night-watch bar is hidden then | OWNER 10-09 (ANC-41) |
| 12.4 | The state lives in the engine; the box shows only what the engine reported; a refused tap says why | DESIGN (ANC-41, as 10.2) |
| 12.5 | A watermaker session is not stopped from the box (it ends with the watermaker; the 95 % HP-pump cut depends on it, D-130) | OWNER 10-09 ('that's fine') (ANC-41) |
| 12.6 | Never during an anchor ALARM; must never delay the anchor watch | DESIGN (ANC-41) |

## 13. HOT WATER tap (NEW - 10-11; Ship's Automation D-135)

| # | Requirement | Source |
|---|---|---|
| 13.1 | A HOT WATER button on the box starts a one-off water-heater run - for the crew's midday cooking, when yesterday's solar-heated water is lukewarm and today's has not started; the box is the only touchscreen aboard | OWNER 10-11 |
| 13.2 | The run ends by itself (not a switch left on): about 20 min at most, earlier when the tank is hot (the engine sees the heater's thermostat stop the draw) or when the heater never draws | OWNER 10-11 ('20 (?) min one off cycle ... without having to remember to turn off the manual switch') / DESIGN (D-135) |
| 13.3 | No start when the house bank is below 50 % - the box says why (and points to the kettle) | OWNER 10-11 ('50% floor for now - we can adjust later') |
| 13.4 | One tap on WATCH, beside FILL (a sixth bottom button); a second tap stops the run; the button shows minutes left, then READY after a run that ended hot | DESIGN (ANC-43) - place accepted by 10-11 ('screen looks fine'; more buttons -> a second row) |
| 13.5 | The state lives in the engine (it switches the heater and ends the run); the box shows only what the engine reported; a refused tap says why | DESIGN (ANC-43, as 12.4) |
| 13.6 | Never during an anchor ALARM (no button then); must never delay the anchor watch | DESIGN (ANC-43, as 12.6) |

## 14. DOCK / LEAVE taps (NEW - 10-15; Ship's Automation D-151)

| # | Requirement | Source |
|---|---|---|
| 14.1 | DOCK on the box arms the slip fence (Ship's Automation WF-108: the boat moves ~65 ft from where she was tapped -> P1 to the crew) - from the nav station, 'maybe under ARM' | OWNER 10-15 |
| 14.2 | DOCK sits on the ARM page beside the rode choices ('DOCK / slip fence'); it is the ENGINE's boat-mode tap, not this box's anchor watch (the box watch stays OFF) | OWNER 10-15 / DESIGN (ANC-44) |
| 14.3 | While the engine says DOCK, WATCH reads 'AT DOCK - slip fence armed', whoever tapped it (box, phone or cockpit tablet) | OWNER 10-15 ('the box shows the truth') / DESIGN |
| 14.4 | LEAVE (confirmed) ends DOCK from the box (boat mode -> MOTOR) - offered ONLY while one of our phones is on the boat Wi-Fi; otherwise the box says to end it from the phone / cockpit tablet. The engine enforces it too (403) | OWNER 10-15 ('disarm only offered if one of our phones is on the local wifi'); D-151 |
| 14.5 | The box may set only DOCK and MOTOR; ending an armed ANCHOR (by DOCK) also needs a crew phone aboard | DESIGN (D-151) |
| 14.6 | The state lives in the engine; the box shows only what the engine reported; a refused tap says why; no link = no DOCK shown, nothing sent | DESIGN (ANC-44, as 12.4 / 13.5) |
| 14.7 | Never during an anchor ALARM (LEAVE replaces ARM only while the box watch is OFF); must never delay the anchor watch | DESIGN (ANC-44, as 13.6) |

## 15. Cold-box EXPECTED? question (NEW - 10-18; Ship's Automation WF-502 / D-128)

| # | Requirement | Source |
|---|---|---|
| 15.1 | When a fridge / freezer has been over its WF-501 limit 10 min with no restock on record, the engine asks 'expected?' (phones = NUDGE, the cockpit page = YES / NO); the anchor box (nav station) asks it too | OWNER 10-18 (OQ-125: phone + cockpit now, the box as a later task) |
| 15.2 | The box shows '<LABEL> warm 10 min - warm food just went in?' with YES (give it 2 h) / NO (alarm now) | OWNER 10-18 (task brief) |
| 15.3 | One tap answers, no confirm | OWNER 10-18 ('y/n') |
| 15.4 | YES = that box's alarm waits 2 h; NO = its P2 alarm fires now; no answer changes nothing | OWNER 10-18 (D-128) |
| 15.5 | One question at a time: one box shown, the rest counted | OWNER (standing rule) / DESIGN (ANC-45) |
| 15.6 | The question lives in the engine; the box asks only while the engine says so and shows its refusal (409 - the alarm is already up) | DESIGN (ANC-45, as 13.5) |
| 15.7 | YES / NO only - no 'not now': a press off the buttons does nothing; the screen stays until answered or the engine stops asking | OWNER 10-18 |
| 15.8 | Takes over like an unacked ALERT (from CLOCK at once, WATCH / FILL after 15 s untouched); an engine ALERT and the anchor ALARM win over it; never delays the anchor watch | DESIGN (ANC-45) |

## 16. Crew RESET on ALERT (NEW - 10-21; Ship's Automation OQ-135 -> D-163)

| # | Requirement | Source |
|---|---|---|
| 16.1 | An engine alarm that may be reset now (clear: manual, input reads normal again - the engine says `rst: true`) can be reset from the box | OWNER 10-21 (OQ-135 queued as recommended) |
| 16.2 | One confirm page before anything is sent ('RESET <rule>? The Pi checked ...' + RESET / CANCEL) | OWNER (Crew-Reset-Proposal, accepted 10-21) |
| 16.3 | The engine decides: it re-checks the input and refuses (409 + why) - the box shows the reason; the box never forces | DESIGN (D-163) |
| 16.4 | Tap-anywhere-to-ACK on ALERT is unchanged; the RESET button and the confirm buttons take their own press | DESIGN (ANC-46) |

## 17. Watch check-in (NEW - 10-05 / 10-21; Ship's Automation WF-614 / D-161)

| # | Requirement | Source |
|---|---|---|
| 17.1 | The engine keeps the watchkeeper clock (sunset underway or a cockpit Start; 15 / 20 / 25 min from the last I'm up) | OWNER 10-05 / 10-21 (D-161) |
| 17.2 | While a check-in is due the box shows it at full brightness and a tap anywhere = I'M UP | OWNER 10-05 (the box is an I'm-up surface) / DESIGN (look - ASSUMED) |
| 17.3 | No buzzer on the box; stage 3 (25 min) is an engine P1 ALERT (horns below once wired) and its ack counts as I'm up | OWNER 10-05 / D-161 |
| 17.4 | The anchor ALARM and an engine ALERT win over it; it never touches the anchor watch, DO0 or DO1 | DESIGN (ANC-47, as 15.8) |

## 4. Open open questions (one at a time)

Answered 10-02: Q2 (2 min), Q1 clock face (large digital, red), Q3 alarm overrides, Q5 dim at night (yes), Q6 (UTC + local zone), Q4 (anchor symbol only, black background), Q6b (zone automatic from position + manual override).

All screen / time questions answered.

(7. Conditions answered 10-02 - 5.11.)
8, 9 ANSWERED 10-02 (no buzzer; DIs open). Open questions now live in CHECKPOINT.md (ANQ-nn).

OQ-098 carry-overs all answered 10-02: conservative-circle floor = rode floor; coarse guard = yes; MSC-10 ~60 ft from the bow (section 5).
