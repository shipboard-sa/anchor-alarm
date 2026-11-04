<!--
Purpose: The box's isolated outputs - what is wired, what drives them, when they switch, why they are OFF at
  power-up, and how the alarm path was proven.
Version: v5 (2026-10-21) - v5: ANQ-18 = before departure (10-21); stale-record pointer -> ANQ-06. v4 (2026-10-07) - v4: DO1 -> AL-13 wired + proven 10-05; S2-DI4 tap wired + proven 10-06; panel terminal
  numbering; DO1 test; who switches the outputs (rebuild-from-spec pass). v3 (2026-10-03) - v3: DO1 = monitoring host down -> AL-13 (ANC-25). v2: DO0 = the panel alarm as wired (ANC-17), circuit from the schematic, MUTE / DISARM
  rule (ANC-23), wiring test 10-02. v1: DO1 panel / DO0 buzzer plan
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page; code in main/watch_ctl.c + hostwatch_ctl.c (when), board.c / ch422g.c (how).
Invocation: n/a (reference doc)
-->

# Outputs and alarm

| Output | Wired to (owner at the box, 10-02) | On when |
|---|---|---|
| **DO0** | with **GND**: the CAMS Master Alarm Panel "Anchor Drag" input (**AL-12**); the Pi taps it on S2-DI4 (Ship's Automation WF-107; W05 tap wired + ground-tested 10-06, F-202). Aqualarm block: terminal 11 | the whole ALARM, MUTE included; released only by DISARM or a fresh ARM / RE-ARM (ANC-23) |
| **DO1** | with **GND**: the panel "Automation" input (**AL-13**; Aqualarm terminal 13) - wired by the owner, same pattern as DO0 -> AL-12; proven aboard 10-05 in all four cases (Cerbo silent, Pi down, network down = no lamp, recovery) - a panel-side input mis-wire found and swapped by the owner during the test | the Pi or the Cerbo is down (ANC-25, [Monitoring-Hosts](Monitoring-Hosts.md)); released after 30 s healthy |

No buzzer is fitted; the box's own alarm is the screen, the panel does the sound.

Terminal numbering (Ship's Automation io-relay-master.md:45, C1, 10-05): the panel inputs have two valid
numberings - docs say AL-n; 'terminal n' is the Aqualarm block's own number (AL-12 = terminal 11, AL-13 = terminal 13).

## Who switches the outputs

Only two places in the firmware drive the outputs: `watch_ctl.c` (DO0 = state is ALARM, every second) and
`hostwatch_ctl.c` (DO1 = Pi OR Cerbo asserted, every second). `net.c` forces both OFF during an OTA upload. The
test builds add `/test/output` (outtest) and the bench toggles (bench); release builds have neither. Nothing on the
network can switch an output in a release build.

## The circuit (Waveshare schematic, 2024-10-23)

CH422G pin OC0 -> R24 -> the LED of PC817 optocoupler T1 -> its transistor pulls the base of output transistor T2
(from 5 V via R58) -> **T2's collector = terminal DO0, emitter = board GND**. So DO0 is an open-collector sink: in
ALARM it pulls the panel line down to GND. Load between DO0 and GND (the terminal "COM" is DI COM, inputs only).
5-36 V, 450 mA, SMAJ36CA surge protection. OC0 LOW = output ON.

## OFF from power-up

* The CH422G powers up with its OC pins high = OFF before the firmware runs.
* The firmware writes the OC register (all OFF) BEFORE the mode and IO registers, so there is no pulse.
* The whole expander is rewritten every 5 s (ANC-16).
* `watch_ctl` sets DO0 from the watch state every second; a restored ALARM (power cut) re-asserts it at once.

## Proven (2026-10-02)

With owner's yes and the panel muted, a one-off signed test build (`sdkconfig.outtest`: `POST /test/output`, at most
15 s, then forced OFF) pulsed DO0 for 15 s at 16:46 - Owner: **the panel's Anchor Drag (AL-12) lit**. The normal
build went straight back (the test command then answers 404). Still to do: the forced full-ALARM run end to end
(a faked drag -> ALARM -> DO0 -> panel -> MUTE -> DISARM), ANQ-18 - BEFORE DEPARTURE (10-21).

DO1 -> AL-13 was proven 10-05 with the monitor test build (`sdkconfig.hwtest`, [Monitoring-Hosts](Monitoring-Hosts.md)).

## Panel side

The panel's Anchor Drag input is a pull-to-ground contact, which DO0 matches. Interface changes are recorded on
both sides (CLAUDE.md). Ship's Automation io_master.yml is updated (AL-12 = DO0, AL-13 = DO1); still wrong:
mx-inv task #900 ('DO1 -> panel AL-12, DO0 buzzer'; spec/04-Interfaces/devices.md:105 quotes it) - ANQ-06, open.
