<!--
Purpose: Every hardware fact the firmware relies on, with its source (clean room, ANC-01), plus the findings
  that changed the spec.
Version: v3 (2026-10-07) - v3: installation / rebuild section, GPIO + expander use table, DO1 now wired to AL-13,
  pointers to the owner's as-built slides and Ship's Automation records (rebuild-from-spec pass). v2 (2026-10-02) - v2: DO0 circuit + terminal block read from the schematic; power + terminals as
  installed (10-02); chip as found by esptool. v1: step 2 facts
Scope: PRODUCTION - doc of the live anchor box (private GitHub repo shipboard-sa/anchor-alarm).
Architecture: anchor-box wiki page; code in main/board.c (pins, timings), ch422g.c, gt911.c.
Invocation: n/a (reference doc)
-->

# Hardware - Waveshare ESP32-S3-Touch-LCD-4.3B

Sources (no others): [W] Waveshare wiki page ESP32-S3-Touch-LCD-4.3B; [S] Waveshare schematic
ESP32-S3-Touch-LCD-4.3B-Sch.pdf (Altium, 2024-10-23); [E] ESPHome `rpi_dpi_rgb` docs, Waveshare 4.3 example;
[C] WCH CH422 datasheet (CH422DS1 EN); [G] Goodix GT911 programming guide; [A] Diodes AP3032 datasheet.

| Item | Fact | Src |
|---|---|---|
| Module | ESP32-S3-WROOM-1-N16R8: 16 MB flash, 8 MB octal PSRAM | W, S |
| Panel | ST7262, 800 x 480 IPS, RGB565 parallel | W |
| RGB pins | HSYNC 46, VSYNC 3, DE 5, PCLK 7; B3-B7 14 38 18 17 10; G2-G7 39 0 45 48 47 21; R3-R7 1 2 42 41 40 | W, E |
| Timings | PCLK 16 MHz; HSYNC pulse 30 / back 30 / front 210; VSYNC 4 / 4 / 4 (pclk_active_neg: bench check) | E |
| I2C | SDA 8, SCL 9 - CH422G, GT911, PCF85063A | W, S |
| Expander | CH422G. EXIO0 = DI0, EXIO1 = CTP_RST, EXIO2 = DISP, EXIO3 = LCD_RST, EXIO4 = SD_CS, EXIO5 = DI1; OC -> OD0 / OD1 -> DO0 / DO1 | S |
| CH422G commands | 48H set system params ([SLEEP]00[OD_EN]0[A_SCAN]0[IO_OE]), 46H write OC3-0, 70H write IO7-0, 4DH read IO; OC high after power-on; open drain = low or no output | C |
| Touch | GT911, INT GPIO 4, reset EXIO1; address 0x5D with INT low at reset release; status 0x814E, point 1 at 0x814F | W, G |
| Backlight | DISP (EXIO2) -> AP3032 boost LED driver CTRL: high = on; CTRL accepts PWM but it is on the I2C expander | S, A |
| Outputs | DO0 / DO1: optocoupler-isolated DRIVE (PC817 inside the board), 5-36 V, sink 450 mA each. Output = NPN open collector: terminal DOUT0 = collector of T2, **emitter on board GND** (not isolated); OC0 -> R24 -> PC817 T1 -> T2 -> DOUT0; OC0 LOW = output ON (sinks to GND). SMAJ36CA TVS on the outputs. **Load: DO0 + GND** (the terminal 'COM' is DI COM - inputs only) | S (read 10-02) |
| Power as installed | From the NMEA 2000 bus (VIN/GND from the N2K drop), N2K bus fed from the Always On Bus (10-02). The board's battery ON/OFF slider is not reachable once mounted | 10-02 |
| Terminal block P1 | VIN, GND, VOUT, GND, SDA, SCL, L (CANL), H (CANH), B, A, DO0, DO1, DI COM, GND, DI0, DI1 (silkscreen order) | S |
| Inputs | DI0 / DI1: 5-36 V, optocoupler, dry / NPN / PNP via DI_COM | W |
| CAN | TX 15, RX 16, TJA1051 transceiver, 120-ohm switch (default off) | W, S |
| RS485 | RX 43, TX 44 | W |
| RTC | PCF85063A, INT GPIO 6, CR927 coin-cell socket | W, S |
| SD | SPI 11 / 12 / 13, CS = EXIO4 | W |
| Buzzer | none on the board | W, S |
| Power | 7-36 V DC or USB-C 5 V; ~450 mA at 5 V typical | W |

## Findings that changed the spec (SPEC section 6)

* Backlight on / off only in practice -> night dimming = dark red digits (ANC-08).
* No buzzer on the board, none fitted (the panel sounds). DO0 + GND = the panel alarm line (10-02); DO1 -> panel AL-13 'Automation' since v0.5.0 (wired + proven 10-05); DI0 / DI1 unused.
* The expander's IO direction is all-or-nothing, so the DI0 / DI1 pins are driven as outputs; the firmware
  does not read the DIs (nothing is wired to them - ANQ-02 answered, 10-02, SPEC 6.4).

## What the firmware uses (rebuild list)

| Resource | Use | Code |
|---|---|---|
| GPIO 8 / 9 | I2C master, 100 kHz, internal pull-ups, shared by CH422G, GT911 (and the unused PCF85063A RTC) | board.c, ch422g.c, gt911.c |
| GPIO 4 | GT911 INT: driven LOW during touch reset (selects address 0x5D), then input | board.c |
| GPIO 46, 3, 5, 7 + 16 data pins | RGB panel HSYNC, VSYNC, DE, PCLK; data B3-B7 14 38 18 17 10, G2-G7 39 0 45 48 47 21, R3-R7 1 2 42 41 40 | board.c |
| GPIO 15 / 16 | TWAI TX / RX -> TJA1051 -> N2K (listen-only) | n2k.c |
| CH422G system register (I2C 0x24) | IO_OE + OD_EN (IO pins outputs, OC pins open-drain) | board.c |
| CH422G OC register (I2C 0x23) | OC0 = DO0, OC1 = DO1, low = output ON; written FIRST at init (0x0F = all OFF) and every 5 s | board.c, ch422g.c |
| CH422G IO register (I2C 0x38) | EXIO1 touch reset, EXIO2 backlight (DISP), EXIO3 LCD reset; EXIO0 / EXIO5 (DI0 / DI1 lines) and EXIO4 (SD CS) held high | board.c |
| Not used | RS485 (43 / 44), SD card, RTC (GPIO 6 INT), DI0 / DI1, USB beyond flashing / log | - |

Panel bring-up order (board.c): I2C bus -> CH422G (outputs OFF first) -> LCD reset pulse (10 ms low) -> touch reset
(INT low, reset low 10 ms, high, INT released, 60 ms) -> GT911 probe (continues without touch if absent) -> RGB
panel (16 MHz PCLK, one PSRAM frame buffer, 800 x 10 px bounce buffer) -> later the backlight on.

## Installation as built

| Item | As built | Source |
|---|---|---|
| Location | on the post beside the nav desk, at the foot of the companionway ladder (NOT the helm - older docs and many file headers say helm, wrong) | 10-06 (CHECKPOINT) |
| Power | VIN / GND on terminal block P1 from the NMEA 2000 drop; the N2K bus is fed from the Always On Bus. Cycling bus power also drops the MSC-10 | 10-02 (Hardware power row); recall: PPT slide 29 'Always On Bus' (projected layout - verify on the slide) |
| N2K data | P1 terminals L (CANL) / H (CANH); on-board 120-ohm switch OFF (metered, ANQ-12) | 10-02; CHECKPOINT ANQ-12 |
| N2K drop cable, connector, shield / drain | not recorded | **ASSUMED - check your wiring** (ANQ-12 also notes the shield single-point ground is still to cite) |
| DO0 | DO0 + GND (P1) -> CAMS Master Alarm Panel input AL-12 'Anchor Drag' = Aqualarm terminal 11; the Pi taps the same line on S2-DI4 (W05, wired + proven 10-06, F-202) | 10-02 (ANC-17); Ship's Automation io-relay-master.md:45 (C1, the two numberings), io_master.yml:76 / :107 |
| DO1 | DO1 + GND (P1) -> panel input AL-13 'Automation' = Aqualarm terminal 13; wired by the owner, proven 10-05 (a panel-side input mis-wire found and swapped by the owner during the test) | CHECKPOINT State (10-05); io_master.yml:3 v1.10, :108 |
| Wire gauge / route / labels of the DO0 / DO1 runs | not recorded | **ASSUMED - check your wiring**; the as-built is the owner's as-built slides - recall hits 'Anchor Drag' on slides 9 (panel), 18 and 19 (nav station, near 'Power 12 VDC', 'YD Electronic Switch'), 20 ('Anchor Drag Monitor') - verify on the slides |
| GND reference | the outputs' emitters are on board GND = the N2K drop's negative, so the panel input and the N2K supply share a ground through the box | schematic [S] (Outputs row) - consequence, not a measured fact |
| Enclosure / mounting hardware | not recorded | **ASSUMED - check your wiring** |
| RTC coin cell (CR927) fitted? | not recorded; the firmware does not use the RTC either way | ASSUMED - check your wiring (only matters if the RTC is ever used) |
| Chip as found | ESP32-S3 rev 0.2, 16 MB flash, 8 MB octal PSRAM, MAC XX:XX:XX:XX:XX:XX | esptool 10-02 (Build-Flash-Test) |

## Replacing the box

Same board (Waveshare ESP32-S3-Touch-LCD-4.3B; the "B" variant has the CH422G expander, isolated DO / DI and CAN).
Move the P1 wires one for one, termination switch OFF, then the first-flash procedure (Build-Flash-Test) - a new
board has a new MAC, so the router reservation for .242 must be changed. A different board or display means new
pin / timing facts, each from a vendor or permissive source and cited here (ANC-01).
