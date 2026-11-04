<!--
Purpose: Everything the anchor box keeps or is configured by - flash partitions, NVS keys and blob layouts, the
  sdkconfig layers, every Kconfig option, the secrets (by name only), and where RAM goes.
Version: v7 (2026-10-23) - v7: token also for POST /watch + /reset (v0.13.0); RAM on v0.13.0; repo path D:\boat\AnchorAlarm. v6 (2026-10-21) - v6: token also for POST /mode + /restock; RAM on v0.12.1 (ANQ-33 closed); ANQ-21 before departure. v5 (2026-10-18) - v5: RESTOCK_POLL_S (ANC-45). v4 (2026-10-11) - v4: HOT_WATER_POLL_S (ANC-43). v3 (2026-10-09) - v3: RAM after the ANC-42 clean-up (LVGL pool in PSRAM, one engine task). v2 (2026-10-09) - v2: FILL_POLL_S + the fill task (ANC-41). v1 (2026-10-07) - v1: new page (rebuild-from-spec pass); read from partitions.csv, sdkconfig.*,
  main/Kconfig.projbuild, watch_ctl.c, boatclock.c, hostwatch_ctl.c, anchorwatch.h at v0.8.0.
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page; the code files above are the truth if they disagree.
Invocation: n/a (reference doc)
-->

# Data and configuration

## Flash layout (16 MB, `partitions.csv` v2)

| Name | Type / subtype | Offset | Size | Holds |
|---|---|---|---|---|
| (bootloader) | - | 0x0 | - | ESP-IDF second-stage bootloader, rollback enabled (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) |
| (partition table) | - | 0x8000 | - | this table |
| nvs | data / nvs | 0x9000 | 0x6000 (24 KB) | the box's settings (below) + ESP-IDF's own (e.g. PHY calibration) |
| otadata | data / ota | 0xF000 | 0x2000 | which app slot boots + its verify state |
| phy_init | data / phy | 0x11000 | 0x1000 | PHY init data |
| ota_0 | app / ota_0 | 0x20000 | 4 MB | app slot A |
| ota_1 | app / ota_1 | 0x420000 | 4 MB | app slot B |

About 7.9 MB of flash is unused. No factory slot and no SPIFFS. v0.6.0 image = 0x171000 bytes (64 % of a slot free).
Changing this table needs a USB flash (an OTA cannot move partitions).

## NVS - namespace `anchorbox`

| Key | Type | Written | Erased | Meaning |
|---|---|---|---|---|
| `armed` | u8 (1) | ARM / RE-ARM (watch_ctl.c `save_armed`) | DISARM | the watch was armed at power-down |
| `rode` | u16, feet | ARM / RE-ARM | DISARM | the rode picked at ARM (0 = "don't know"); re-used to re-arm fresh after a power cut with no circle yet |
| `snap` | blob `aw_snapshot_t` (48 bytes) | every watch STATE change while LOCKED / CONSERVATIVE / ALARM (a handful per anchorage) | on a state change to OFF / ACQUIRING (DISARM, ARM) | the circle, restored at power-up with no age limit (ANC-22) |
| `zone_pin` | string, max 5 chars (`CT`, `ET`, `BS`, `AT`, `Z`, `N-4`, `N-5`, `N-6` ...) | ZONE page pin | ZONE -> AUTO | the pinned time zone ('M' on the clock) |
| `cerbo_tab` | blob (296 bytes) | whenever the learned table changes | never (rebuilt from claims) | the Cerbo's N2K addresses + NAMEs (ANC-25) |

`aw_snapshot_t` (anchorwatch.h, `AW_SNAP_VERSION` 1): `u8 version, u8 state (2 LOCKED / 3 CONSERVATIVE / 4 ALARM),
u8 has_centre, u8 locked`, then doubles `lat, lon` (the frozen centre, or the arm point for a coarse-guard ALARM),
`radius` (m), `alarm_radius` (m), `rode_ft`. A blob of the wrong size or version is ignored (the box then re-arms
fresh if `armed` is set). **Bump `AW_SNAP_VERSION` whenever the struct changes**, or an old blob could be read as
the new layout.

`cerbo_tab` blob: `u8 n` (<= 32), `u8 sa[32]`, `u64 name[32]` (natural C alignment; same compiler both ends).

Power-up order: NVS opened in `net_start` (erased and re-initialised only if IDF reports no free pages / a new
NVS version), then `watch_ctl_start` reads `armed`, `rode`, `snap`; `hostwatch_ctl_start` reads `cerbo_tab`; the
clock task reads `zone_pin`. Nothing secret is in NVS (Wi-Fi login lives in the image, `WIFI_STORAGE_RAM`).
Restoring NVS is never needed: losing it costs a re-ARM, a re-pin of the zone and a wait for Cerbo claims.

## Build configuration layers

`idf.py` merges `SDKCONFIG_DEFAULTS` left to right into a generated `sdkconfig` (gitignored). Edit the layers,
never `sdkconfig`; delete `sdkconfig` (or the build dir's) after editing a layer, and run `reconfigure` after
editing `main/Kconfig.projbuild` (CHECKPOINT gotchas).

| Layer | In git | Used by | Holds |
|---|---|---|---|
| `sdkconfig.defaults` | yes | every build | target + board, partition table, PSRAM, LVGL fonts, signed OTA + rollback, Wi-Fi / lwIP buffer placement, socket pool 16, `ANCHORBOX_PI_WATCH=y` |
| `sdkconfig.secrets` | **no** (gitignored) | every boat build, always second | `CONFIG_ANCHORBOX_WIFI_SSID`, `CONFIG_ANCHORBOX_WIFI_PASSWORD`, `CONFIG_ANCHORBOX_NIGHT_WATCH_TOKEN` (the box token, same value as the engine's NIGHT_WATCH_TOKEN). Values never written in any doc |
| `sdkconfig.bench` | yes | bench build only | `ANCHORBOX_DEMO_CLOCK=y`, `ANCHORBOX_BENCH_TESTS=y` |
| `sdkconfig.outtest` | yes | output-test build only | `ANCHORBOX_REMOTE_OUTPUT_TEST=y` |
| `sdkconfig.hwtest` | yes | monitor-test build only | `ANCHORBOX_HOSTWATCH_TEST=y` |

### `sdkconfig.defaults` - the choices and why

| Setting | Why |
|---|---|
| `IDF_TARGET esp32s3`, `ESPTOOLPY_FLASHSIZE_16MB`, `SPIRAM` + `SPIRAM_MODE_OCT` + `SPIRAM_SPEED_80M` | the ESP32-S3-WROOM-1-N16R8 module (16 MB flash, 8 MB octal PSRAM) - Hardware |
| `SPIRAM_XIP_FROM_PSRAM`, `LCD_RGB_ISR_IRAM_SAFE` | the usual ESP-IDF settings for an RGB panel with its frame buffer in PSRAM: code runs from PSRAM and the panel ISR stays in IRAM, so the display keeps refreshing while flash is written (NVS, OTA). The project never recorded its own reason - this is Espressif's RGB-LCD guidance |
| `ESP_DEFAULT_CPU_FREQ_MHZ_240`, `FREERTOS_HZ 1000` | full speed; 1 ms ticks for LVGL timing |
| `LV_COLOR_DEPTH_16`, `LV_FONT_MONTSERRAT_20/28/48`, `LV_USE_ARC` | RGB565 panel; the three font sizes used; arcs for the plot ring / symbol |
| `PARTITION_TABLE_CUSTOM` -> `partitions.csv` | two OTA slots (SPEC 8.3) |
| `SECURE_SIGNED_APPS_NO_SECURE_BOOT`, `SECURE_SIGNED_APPS_RSA_SCHEME`, `SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`, `SECURE_BOOT_BUILD_SIGNED_BINARIES`, `SECURE_BOOT_SIGNING_KEY "keys/anchorbox_signing_key.pem"` | every built app is signed (RSA-3072, ANC-18); an OTA image is accepted only if its signature verifies against the RUNNING app's key. No hardware Secure Boot / eFuses burnt - a USB flash can still replace anything (recovery stays possible) |
| `BOOTLOADER_APP_ROLLBACK_ENABLE` | a new image boots PENDING_VERIFY and rolls back on any reset before it is marked good (30 s) |
| `LWIP_LOCAL_HOSTNAME "anchor-box"` | DHCP / mDNS name |
| `SPIRAM_TRY_ALLOCATE_WIFI_LWIP`, `ESP_WIFI_STATIC_RX_BUFFER_NUM 5`, `ESP_WIFI_RX_BA_WIN 8`, `ESP_WIFI_DYNAMIC_TX_BUFFER`, `ESP_WIFI_DYNAMIC_RX_BUFFER_NUM 16` | Wi-Fi and lwIP buffers in PSRAM so the display keeps its internal DMA RAM (first flash 10-02: Wi-Fi + 40-line buffers did not fit) |
| `ANCHORBOX_PI_WATCH=y` | Pi watch ON since the cut-over (ANC-29) |
| `LWIP_MAX_SOCKETS 16` | was 10; the HTTP server alone may hold 10 (ANC-30) |

## Kconfig options (`main/Kconfig.projbuild`, menu "Anchor box")

| Option | Default | Effect |
|---|---|---|
| `ANCHORBOX_DEMO_CLOCK` | n | BENCH: made-up time from uptime starting 12:34. Never on the boat |
| `ANCHORBOX_BENCH_TESTS` | n | BENCH: DO0 / DO1 toggles on WATCH |
| `ANCHORBOX_REMOTE_OUTPUT_TEST` | n | TEST: `POST /test/output` |
| `ANCHORBOX_HOSTWATCH_TEST` | n | TEST: `POST /test/hostwatch` |
| `ANCHORBOX_PI_WATCH` | n (y in defaults) | poll the Pi's SignalK health |
| `ANCHORBOX_PI_HOST` | 192.168.1.10 | the Pi: SignalK AND the rules engine |
| `ANCHORBOX_PI_PORT` | 3000 | SignalK HTTP |
| `ANCHORBOX_PI_HEALTH_PATH` | `/signalk/v1/api/vessels/self/automation/health/healthy` | |
| `ANCHORBOX_PI_FALLBACK_PATH` | `/signalk/v1/api/vessels/self/navigation/position` | used only when the health path answers 404 |
| `ANCHORBOX_PI_POLL_S` | 10 | |
| `ANCHORBOX_PI_HEALTH_FRESH_S` | 90 | engine keep-alive is 60 s, so 60 would flap |
| `ANCHORBOX_PI_FALLBACK_FRESH_S` | 60 | |
| `ANCHORBOX_PI_DOWN_AFTER_S` | 60 | unhealthy this long -> router ping -> DO1 |
| `ANCHORBOX_ROUTER_HOST` | 192.168.1.1 | |
| `ANCHORBOX_CERBO_SILENT_S` | 180 | |
| `ANCHORBOX_CERBO_MFR` | 358 | Victron Energy's N2K manufacturer code |
| `ANCHORBOX_HOSTWATCH_RELEASE_S` | 30 | healthy this long -> DO1 released |
| `ANCHORBOX_ENGINE_PORT` | 8200 | rules engine API |
| `ANCHORBOX_NIGHT_WATCH_TOKEN` | "" | the box token (secrets only); also used for POST /ack since v0.8.0, POST /fill (ANC-41), POST /hot-water (ANC-43), POST /mode (ANC-44), POST /restock (ANC-45), POST /watch (ANC-47) and POST /reset/<rule> (ANC-46) |
| `ANCHORBOX_NIGHT_WATCH_POLL_S` | 5 | |
| `ANCHORBOX_ALERTS_POLL_S` | 2 | engine alerts; a tap acks (ANC-33) |
| `ANCHORBOX_FILL_POLL_S` | 10 | tank-fill sessions (ANC-41); 2 s while the FILL page is open |
| `ANCHORBOX_HOT_WATER_POLL_S` | 10 | the engine's hot-water run (ANC-43); a tap posts at once |
| `ANCHORBOX_RESTOCK_POLL_S` | 10 | the engine's cold-box 'expected?' question (ANC-45); YES / NO posts at once |
| `ANCHORBOX_WATCHKEEPER_POLL_S` | 5 | the engine's watchkeeper timer, GET /watch (ANC-47, WF-614); a tap posts I'M UP at once |
| `ANCHORBOX_WIFI_SSID` / `_PASSWORD` | "" | secrets only; empty SSID = Wi-Fi off |

Fixed in code (not Kconfig): the anchor-watch parameters (`aw_default_config`, [Anchor-Watch-Algorithm](Anchor-Watch-Algorithm.md)),
`bus_quiet_s` 10 (hostwatch_ctl.c), WATCH -> CLOCK 2 min (screens.h), alert take-over 15 s / clock hold 2 min
(alerts.h), zone steadiness 300 s, time agreement 2 s, night altitude -0.833 deg (boattime.h), breadcrumbs 2160
points = 6 h at 10 s (watch_ctl.h), expander refresh 5 s and health mark 30 s (boatclock.c).

## Secrets and private files (names and places only)

| Item | Where | In git | If lost |
|---|---|---|---|
| Signing key `anchorbox_signing_key.pem` (RSA-3072 private key) | `D:\boat\AnchorAlarm\keys\` on the laptop | never | the box refuses every OTA built with another key -> next update needs USB with a new key at the boat. Off-boat copy: CHECKPOINT ANQ-21 (before departure, 10-21; a password manager) |
| Wi-Fi login, box token | `D:\boat\AnchorAlarm\sdkconfig.secrets` | never | re-enter (SSID 'YourBoatWiFi'; password from the owner; token = the engine's NIGHT_WATCH_TOKEN on the Pi) |
| Built images | `build*/anchor_box.bin` - contain the Wi-Fi password and token in clear | never | rebuild |
| the original firmware image | `backup/anchorbox-original-backup.bin` (SHA-256 001EACB2...147A) | never (proprietary) | only needed to restore the original firmware; off-boat copy ANQ-21 |

## RAM

Internal RAM was the scarce resource (28 KB free, lowest 20 KB on v0.9.0; 31 / 23-26 on v0.6-v0.8; ~72 KB was v0.4.0);
since v0.9.1 ~102-104 KB free, lowest 67-70 KB (v0.13.0 at 49.7 h, 10-23: 102 / 67).
**ANC-42 (10-09) freed ~80 KB of it statically** (`idf.py size` DIRAM used 201,337 -> 121,337 bytes): LVGL's 64 KB
built-in heap and the lwip / Wi-Fi libraries' .bss now live in PSRAM (EXT_RAM_BSS_ATTR,
`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`). Internal RAM still holds the two LVGL draw buffers (2 x 800 x 20 x 2
bytes = 2 x 32 KB, DMA-capable), the core tasks' stacks, and the RGB bounce buffer (800 x 10 px). Everything big goes
to PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`): the panel frame buffer, the LVGL heap, the watch state `aw_t`
(~25 KB), breadcrumbs, the engine reply buffer, the alert view, the engine task's stack. Note: plain `malloc` of
<= 16 KB still tries internal RAM first (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`) - use heap_caps for buffers. Watch the status line `internal RAM free ... (lowest ...)` after every feature (ANQ-33, closed 10-11).

| Task | Priority | Stack | Period |
|---|---|---|---|
| n2k | 5 | 4 KB | on frame notify (max 1 s) |
| httpd (IDF) | 5 (IDF default) | 12 KB | on request |
| ui (LVGL, core 1) | 4 | 8 KB | 5-30 ms |
| watch | 4 | 6 KB | 1 s |
| clock | 3 | 6 KB | 1 s |
| hostwatch | 2 | 6 KB | 1 s (Pi poll 10 s, first at 15 s) |
| engine | 1 | 6 KB, PSRAM | from 12 s: night watch 5 s, alerts 2 s, fill 10 s (2 s with the FILL page open), hot water 10 s, in turn; a tap kicks it |
