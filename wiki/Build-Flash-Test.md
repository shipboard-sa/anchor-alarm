<!--
Purpose: How to build the anchor box firmware and the reference model, run every test, update the live box over
  Wi-Fi, use the bench / test builds, and flash or recover it by USB.
Version: v6 (2026-10-23) - v6: inventory to v0.13.0 (alerts 66, wkeep 32); aboard row (EXPECTED? proven 10-23); repo path D:\boat\AnchorAlarm. v5 (2026-10-21) - v5: inventory to v0.12.1 (pytest 185); aboard row + ANQ status per the 10-21 regroup. v4.1 (2026-10-18) - v4.1: host-test inventory brought up to the 9 programs (fill, hotwater, boatmode, restock rows). v4 (2026-10-07) - v4: blank-PC setup, first flash of a new board, signing key handling, release checklist,
  test inventory to v0.8.0 (rebuild-from-spec pass). v3 (2026-10-03) - v3: monitor test build (sdkconfig.hwtest), Kconfig edits need `idf.py reconfigure`. v2: signed OTA is the normal path; host tests (zig cc); release = tag + clean build;
  test builds; what the first flash found and did. v1: first-flash plan
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page; mirrors the run section of ../CHECKPOINT.md and ../README.md.
Invocation: n/a (reference doc)
-->

# Build, test, update

## Tests (run before every release)

```
cd D:\boat\AnchorAlarm\reference
.venv\Scripts\python -m pytest                      # 163 tests, ~10 s, offline
cd ..
test_host\run.bat                                   # C ports vs the reference: 9 host test programs (inventory below)
```

`run.bat` regenerates the vectors from `reference/` (`tools/make_c_vectors.py`, `make_watch_vectors.py`,
`make_hostwatch_vectors.py`) and compiles `main/boattime.c`, `anchorwatch.c`, `hostwatch.c`, `nightwatch.c` and
`alerts.c` on the PC with zig cc (pip package `ziglang` in the venv).
New venv: `python -m venv .venv; .venv\Scripts\python -m pip install -r requirements.txt`.

Sim / replay: `.venv\Scripts\python -m anchorwatch sim --out swing.gpx --pattern steady --hours 2 --drag slow`
then `... replay swing.gpx --rode-ft 100`.

### Test inventory (v0.13.0)

| Suite | Command | What it pins |
|---|---|---|
| Reference pytest | `reference\.venv\Scripts\python -m pytest` | 185 tests (10-08, with the heading lock ANC-40; 163 at v0.8.0), offline: geo + circle fit, the watch state machine, rode floor / coarse guard / DRAG?, power-cut restore, replay, the 2021 Chesapeake real track (no false alarm), boat time vs IANA tz + astral, the host watch |
| boattime | `test_host\test_boattime.exe` (via run.bat) | 14,372 checks: zones, DST, nautical, steadiness, pin, GPS time acceptance, sun / night vs the Python |
| watch | `test_watch.exe` | the 20 scenario vectors: same events at the same seconds, same final circle (Anchor-Watch-Algorithm) |
| hostwatch | `test_hostwatch.exe` | 11 SignalK reply parse cases + 32 scenarios / 46,970 observations vs `hostwatch.py` |
| nightwatch | `test_nightwatch.exe` | 30 fixed cases: /night-watch reply parsing, bar faces, `nw_bar_shown` (night only unless ARMED / nudging) |
| alerts | `test_alerts.exe` | 66 fixed cases: /alerts reply parsing (escapes, braces in text, truncation, missing keys, `rst`), take-over / release / tap / footer / CLOCK + WATCH lines (panel-LED model), which alert gets RESET (ANC-46) |
| fill | `test_fill.exe` | 57 fixed cases: /fill reply parsing, FILL page rows / tab / tap rules (ANC-41) |
| hotwater | `test_hotwater.exe` | 29 fixed cases: /hot-water reply + 409 parsing, HOT WATER button / notice, tap rule (ANC-43) |
| boatmode | `test_boatmode.exe` | 32 fixed cases: /mode reply + 403 parsing, DOCK / LEAVE rules, AT DOCK lines (ANC-44) |
| restock | `test_restock.exe` | 66 fixed cases: /restock reply + 409 parsing, which box is asked, take-over / release / tap rules, words, POST body (ANC-45) |
| wkeep | `test_wkeep.exe` | 32 fixed cases (a live /watch reply as fixture): /watch reply parsing, when the check-in shows, stage words, tap rule, POST body (ANC-47) |
| Aboard | test builds with owner's yes, panel muted | DO0 -> AL-12 (10-02), DO1 -> AL-13 all four cases (10-05), night-watch tap (10-06, F-200), TEST WF-303 on ALERT + ack (10-07), LEAVE -> MOTOR + DOCK re-armed from the box (10-15, ANQ-38), first real EXPECTED? answer (Engel, 10-23 21:05 CDT, ANQ-39). Not yet: forced full ALARM (ANQ-18, before departure), RESET on a test alarm + WATCH CHECK-IN underway (ANQ-40), power cycle (ANQ-19, parked), a real anchorage (ANQ-04, parked) |

Not covered by any automated test: the UI layout (ui_*.c), net.c / OTA, n2k.c decoding, board drivers - these are
proven only on the box.

## Release and update (normal path - signed OTA over Wi-Fi)

ESP-IDF v6.0.3 at `D:\esp\esp-idf`, tools at `D:\esp\.espressif`.

1. Commit, then tag (`git tag -a v0.N.0 -m "..."`) - the box shows `git describe` as its version.
2. Build: `$env:IDF_TOOLS_PATH='D:\esp\.espressif'; . D:\esp\esp-idf\export.ps1` then
   `idf.py --no-ccache "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.secrets" build`
   (signed automatically with `keys/anchorbox_signing_key.pem`).
3. With the owner's go-ahead and the watch DISARMED (the box refuses otherwise):
   `python tools\ota_push.py --host 192.168.1.100`.
4. Check `--status`: `running` changed slot, version right, clock / watch lines sane. A build that dies within 30 s
   rolls back by itself; after 30 s it is marked good.

What the box accepts: only images signed by our key (`ESP_ERR_OTA_VALIDATE_FAILED` / "signature bad" otherwise -
tested 10-02 with an unsigned image, which was refused with no restart).

## From a blank Windows PC

| Need | Version / place | Notes |
|---|---|---|
| ESP-IDF | **v6.0.3** (dependencies.lock pins `idf 6.0.3`), at `D:\esp\esp-idf`; tools at `D:\esp\.espressif` | nothing on C:. How it was installed (Espressif installer vs `git clone --recursive -b v6.0.3` + `install.ps1 esp32s3`) is not recorded - ASSUMED - check your wiring; either gives the same tree. Set `$env:IDF_TOOLS_PATH='D:\esp\.espressif'` BEFORE `. D:\esp\esp-idf\export.ps1` |
| Components | `lvgl/lvgl` 9.2.2, `espressif/mdns` 1.14.0 | fetched by the IDF component manager into `managed_components/` on the first build, pinned by `dependencies.lock` (hashes in the file) |
| git | any | clone `https://github.com/shipboard-sa/anchor-alarm` (PRIVATE, the owner's account) into `D:\boat\AnchorAlarm` (was code/anchor-box before 10-20) |
| Python for the reference model | 3.14 (reference/README.md) in `reference\.venv` | `python -m venv .venv; .venv\Scripts\python -m pip install -r requirements.txt` -> pytest >= 9, websockets >= 15, tzdata, astral >= 3, ziglang (the host C compiler for test_host) |
| Private files (not in git) | `keys\anchorbox_signing_key.pem`, `sdkconfig.secrets` | copy from the owner's off-laptop copies (ANQ-21) - never from GitHub, they are not there |
| USB driver | none - the ESP32-S3's native USB shows as a COM port (COM6 on the laptop, VID 303A) | |

`sdkconfig.secrets` holds exactly three lines, values never written down here:
`CONFIG_ANCHORBOX_WIFI_SSID="..."`, `CONFIG_ANCHORBOX_WIFI_PASSWORD="..."`,
`CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN="..."` (= the engine's NIGHT_WATCH_TOKEN). Delete `sdkconfig` after editing it.

Check the setup: `test_host\run.bat` passes, then the release build ends with a signed `build\anchor_box.bin`.
Builds that segfault in the compiler (seen on esp_lcd_panel_rgb.c, 10-02) go through with `--no-ccache`.

## The signing key

* **What:** an RSA-3072 private key (Secure Boot V2 scheme, `CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME`) at
  `keys/anchorbox_signing_key.pem`. Every build signs the app with it (`SECURE_BOOT_BUILD_SIGNED_BINARIES`). The box
  takes an OTA image only if it verifies against the key of the app it is running.
* **No eFuses are burnt** (`..._NO_SECURE_BOOT`): USB can always reflash the board, with any key or none.
* **Making a new one** (only when the old one is lost, or deliberately rotated): ESP-IDF's
  `espsecure.py generate_signing_key --version 2 --scheme rsa3072 keys\anchorbox_signing_key.pem`. The command used
  on 10-02 is not recorded - ASSUMED - check your wiring if it matters. A new key means the next install must be by USB at
  the boat (the running image only accepts the old key); after that, OTA works again with the new key.
* **Rotating the key over OTA** (e.g. an image signed with both keys) has never been tried on this box - treat any
  key change as a USB job.
* Copies: laptop + off-boat (ANQ-21, before departure - 10-21). Never in git, never on GitHub.

## First flash of a NEW board (replacement box)

At the boat with the owner, USB-C to the laptop. The board as bought runs Waveshare's demo or someone else's firmware.

1. Back up whatever is on it: `python -m esptool --chip esp32s3 -p COM6 -b 921600 read-flash 0 0x1000000 backup.bin`
   then `verify-flash 0 backup.bin`.
2. Erase: `python -m esptool --chip esp32s3 -p COM6 erase-flash` (clears foreign NVS - the previous Wi-Fi / OTA settings
   were removed this way 10-02).
3. Release build (commit + tag first), then
   `idf.py --no-ccache "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.secrets" -p COM6 -b 921600 flash`
   (writes bootloader 0x0, partition table 0x8000, otadata, app -> ota_0).
4. Watch the log once: `idf.py -p COM6 monitor` - expect `ch422g: sys ... io ... oc 0xf` (outputs OFF),
   `gt911: product id '911'`, `n2k: listen-only on GPIO 15/16 at 250000 bit/s: ESP_OK`, `net: Wi-Fi up: 192.168.1.x`.
5. Router: give the new MAC the .242 reservation (AnchorAlarm) - a new board has a new MAC; update
   Ship's Automation spec/04-Interfaces/network.md. Reboot the box.
6. Checks with the owner (panel muted, his yes for each): touch axes, `--status` lines, the wiring test build for DO0 /
   DO1 (sdkconfig.outtest, Outputs-and-Alarm), then the release build back.
7. Re-pin the zone if it was pinned; the Cerbo table refills from address claims (or stays empty until one is heard).

From then on: signed OTA only.

## Release checklist (every update)

1. Behaviour change in `reference/` first, tests green: `reference\.venv\Scripts\python -m pytest`.
2. `test_host\run.bat` green (C ports vs the reference).
3. Update the docs that change (CHECKPOINT, the wiki page, Design-Decisions row, file headers' Version).
4. `git commit`, `git tag -a v0.N.0 -m "..."` - the box reports `git describe`; a dirty tree shows `-dirty`.
5. Release build (no bench / test overlay): `idf.py --no-ccache "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.secrets" build`.
   After a Kconfig edit: `reconfigure` first and grep the generated `sdkconfig` for the new option.
6. **the owner's go-ahead**, watch DISARMED: `python tools\ota_push.py --host 192.168.1.100`. Exit codes: 0 installed and
   answering, 1 box not reachable, 2 refused (409 armed / 400 bad image or signature), 3 no answer after the restart.
7. `--status`: `running` changed slot, `version` = the tag, `clock` / `watch` sane, `pi OK`, `cerbo OK`,
   `night watch ... link ok, token set`, `alerts ... link ok | ack token set`, RAM free vs the last release.
   Wait past 30 s (the image is then final - Operations).
8. Push the commit + tag to GitHub (`git status` first: no keys / secrets / backup / build).

## Bench and test builds - never left on the box

| Build | Overlay | What it adds |
|---|---|---|
| bench | `sdkconfig.bench` | made-up time from 12:34, DO0 / DO1 toggles on WATCH |
| output test | `sdkconfig.outtest` | `POST /test/output?do=0&s=10` - pulses an output, max 15 s, then OFF |
| monitor test | `sdkconfig.hwtest` | `POST /test/hostwatch?pi=down\|ok&router=down\|up&cerbo=silent&bus=silent` - fakes the monitor inputs for 10 min (real thresholds) to prove DO1 -> AL-13 (ANQ-23) |

Build each in its own dir, e.g. `idf.py --no-ccache -B build-outtest "-DSDKCONFIG=build-outtest/sdkconfig"
"-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.secrets;sdkconfig.outtest" build`, push with
`--bin build-outtest\anchor_box.bin`, and push the normal `build\anchor_box.bin` straight back after the test.
Any output test needs owner's yes for that test.

**After editing `main/Kconfig.projbuild`** run `idf.py ... reconfigure` (same `-B` / `-DSDKCONFIG*` arguments) before
building: on 10-03 a plain `build` did not pick up a new option, and nothing warned. Check the generated
`sdkconfig` has the option before trusting the image.

## USB (layout change, recovery) - at the boat, with the owner

The box shows up as COM6 (Espressif USB, VID 303A). Logs come over the same cable.

* Backup first: `python -m esptool --chip esp32s3 -p COM6 -b 921600 read-flash 0 0x1000000 backup.bin`, then
  `verify-flash 0 backup.bin`.
* Full flash: `idf.py --no-ccache "-DSDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.secrets" -p COM6 -b 921600 flash`.
* Back to the original firmware: `esptool --chip esp32s3 -p COM6 write-flash 0 backup/anchorbox-original-backup.bin`
  (restores his Wi-Fi / OTA settings too).

## What the first flash found (2026-10-02)

* Chip: ESP32-S3 rev 0.2, 16 MB flash, 8 MB octal PSRAM, MAC 98:a3:16:d9:ee:2c.
* the original layout: nvs, otadata, ota_0 / ota_1 (6 MB each), spiffs. His NVS held a Wi-Fi login and
  `gps_url` = the Pi at 192.168.68.51:10110 - his firmware took GPS over Wi-Fi, not N2K. Backed up
  (`backup/`, SHA-256 001EACB2...147A, verified against the chip), then the NVS and later the whole chip erased.
* Touch works on our firmware (`gt911: product id '911'`, axes right) - the original firmware was the fault.
* Memory lessons: Wi-Fi + 2 x 40-line draw buffers did not fit the internal RAM - the display now starts first with
  2 x 20 lines, Wi-Fi buffers live in PSRAM, and a Wi-Fi failure is never fatal. The RSA signature check needs a
  12 KB HTTP task stack (4 KB crashed).
