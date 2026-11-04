<!--
Purpose: How the box knows the time - UTC from N2K (two-message acceptance), the local zone from position (area
  table, US daylight saving, nautical fallback, 5-min steadiness) with a manual pin, and night (sun altitude).
Version: v5 (2026-10-07) - v5: zone ids, time bases, source note (rebuild-from-spec pass). v4 (2026-10-02) - v4: ZONE pin control live on WATCH (kept in NVS). v3: BUILT + live on the box (step 3). v2: Pensacola is US Central (was wrongly Eastern)
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page. Model: reference/anchorwatch/boattime.py (tests vs IANA tz + astral);
  firmware: main/boattime.c (host-tested: test_host/run.bat) fed by main/n2k.c, drawn by app_main.c.
Invocation: n/a (reference doc)
-->

# Time and zones (step 3 - built, live 10-02)

## UTC (ANC-19)

From the MSC-10 (N2K source 3) on NMEA 2000, listen-only: PGN 126992 System Time, ~1 per second, source kind
0 = GPS. The clock is set only when **two consecutive messages agree** (within 2 s of the box's own elapsed time),
kind 0 only, and not before 2026-01-01. Between messages the box runs on its own timer. A single wild message is
ignored; a real jump needs two agreeing messages. Until then the clock shows "--:--" (ANC-07).
The on-board PCF85063A RTC is still not used: after a power cut the clock is "--:--" until GPS time returns.

Sources: n2k.c accepts 126992 and 129025 from ANY source address; on this bus only the MSC-10 (src 3) sends them
(Ship's Automation F-135). A second time / position source would be mixed in - see [Operations](Operations.md) and
ANC-37.

## Local zone (ANC-07, ANC-20)

Automatic from position (PGN 129025), first match wins; positions older than 5 min are not used:

| Area (box) | Zone id | Offset | Daylight saving |
|---|---|---|---|
| Florida from Port St. Joe east, Keys, east coast (24-31 N, 85.39-79.6 W) | ET | UTC-5 | US rule |
| Northern Gulf: Texas, NOLA, Pensacola, Mexico Beach (25-31.5 N, 97.6-85.39 W) | CT | UTC-6 | US rule |
| Bahamas + Turks & Caicos (20.9-27.5 N, 80.6-71 W) | BS | UTC-5 | US rule |
| DR, PR, VI, ownerwards, Windwards (13-20.5 N, 71.6-59 W) | AT | UTC-4 | none |
| ABC islands, Grenada, Trinidad (10-13 N, 71-59 W) | AT | UTC-4 | none |
| Anywhere else | N-n | longitude / 15, rounded | none |

US rule: 2nd Sunday in March 02:00 standard -> 1st Sunday in November 02:00 daylight (2026: 8 Mar, 1 Nov).
Coastal Central / Eastern line = Bay / Gulf county line between Mexico Beach (Central) and Port St. Joe (Eastern).
Checked against the IANA tz database at 32 harbours, every hour around each change 2026-2030.

Known limits: Cuba's north coast falls in the Bahamas box (Cuba changes on the same days at other hours); Haiti and
Colombia are outside the boxes (not on the itinerary). The boxes include nearby open water on purpose (ANC-20).

A new zone is followed only after it has held for **5 minutes**, so swinging at anchor across a line never
flickers the clock. Pin (SPEC 3.5): a fixed zone (any id above, or a nautical N-n) until AUTO again; the clock
shows 'M'. Set from the WATCH screen's ZONE page (AUTO, CT, ET, BS, AT, UTC-4/-5/-6), kept across power-ups.

### Zone ids

The pin is stored in NVS `zone_pin` as the zone id: `CT` (UTC-6, US DST), `ET` (UTC-5, US DST), `BS` (UTC-5, US
DST), `AT` (UTC-4, none), `Z` (UTC), or a nautical `N-n` / `N+n` (n = 1..12, no DST). The ZONE page offers AUTO,
US Central, US Eastern, Bahamas, Atlantic, UTC-4, UTC-5, UTC-6 (the last three = `N-4`, `N-5`, `N-6`). Area boxes are
lat / lon rectangles tested in table order (first match wins): ET 24-31 N x 85.39-79.6 W; CT 25-31.5 N x
97.6-85.39 W; BS 20.9-27.5 N x 80.6-71.0 W; AT 13-20.5 N x 71.6-59.0 W; AT 10-13 N x 71.0-59.0 W (boattime.c).

## Night (ANC-08, ANC-21)

Night = geometric sun altitude below -0.833 deg (sunset / sunrise) at the boat's position, NOAA Solar Calculator
method (matches astral to 0.05 deg). The clock red drops to dark red. No position -> treated as day.
An ALARM always goes to full brightness.

## Two clocks on the box

* **Boat time** (this page) - GPS UTC + zone - is used for the CLOCK face, night, and SignalK timestamp freshness
  ([Monitoring-Hosts](Monitoring-Hosts.md)).
* **Uptime** (`esp_timer`) is used for everything that must work without GPS time: the anchor watch (fix times =
  receive times, ANC-36), debounces, poll intervals, alert ages (the engine sends `age_s`, so the box needs no clock
  for it). A GPS time jump therefore cannot trip or hide an anchor alarm.

## Checking it without the USB cable

`python tools\ota_push.py --host 192.168.1.100 --status` shows e.g. `clock 15:54 zone CT (UTC-300 min) day`,
or why it shows "--:--" (GPS time / position missing).
