# Purpose: Component file for the anchor box firmware sources.
# Version: v11 (2026-10-21) - v11: wkeep.c + wkeep_ctl.c + ui_wkup.c (ANQ-27). v10 (2026-10-18) - v10: restock.c + restock_ctl.c + ui_ask.c (ANC-45). v9 (2026-10-15) - v9: boatmode.c + boatmode_ctl.c (ANC-44). v8 (2026-10-11) - v8: hotwater.c + hotwater_ctl.c (ANC-43). v7 (2026-10-09) - v7: engine_link.c (ANQ-33); LVGL pool placed in PSRAM. v6 (2026-10-09) - v6: fill.c + fill_ctl.c + ui_fill.c (ANC-41). v5 (2026-10-07) - v5: alerts.c + alerts_ctl.c + ui_alert.c (ANQ-10). v4 (2026-10-06) - v4: nightwatch.c + nightwatch_ctl.c (ANQ-30). v3: hostwatch.c + hostwatch_ctl.c (ANC-25), esp_http_client + lwip. v2: step 3-4 sources (n2k, net, boattime, anchorwatch, watch_ctl, boatclock)
# Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
# Architecture: <your-project-dir>/main - board, display (LVGL glue), screens, UI.
# Invocation: read by idf.py build.
idf_component_register(
    SRCS "app_main.c" "board.c" "ch422g.c" "gt911.c" "display.c" "screens.c" "ui_clock.c" "ui_watch.c" "n2k.c" "net.c" "boattime.c" "anchorwatch.c" "watch_ctl.c" "boatclock.c" "hostwatch.c" "hostwatch_ctl.c" "nightwatch.c" "nightwatch_ctl.c" "alerts.c" "alerts_ctl.c" "ui_alert.c" "fill.c" "fill_ctl.c" "ui_fill.c" "engine_link.c" "hotwater.c" "hotwater_ctl.c" "boatmode.c" "boatmode_ctl.c" "restock.c" "restock_ctl.c" "ui_ask.c" "wkeep.c" "wkeep_ctl.c" "ui_wkup.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client lwip esp_wifi esp_netif esp_event esp_http_server app_update nvs_flash esp_app_format esp_driver_twai esp_driver_i2c esp_driver_gpio esp_lcd esp_timer esp_psram)
