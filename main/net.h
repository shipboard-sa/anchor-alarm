/**
 * Purpose: Wi-Fi + signed OTA updates for the anchor box (SPEC 8): joins the boat Wi-Fi, answers as
 *   anchor-box.local, accepts a new firmware image on POST /ota ONLY if it is signed with the owner's key, never while
 *   the watch is armed or an ALARM shows; outputs OFF throughout.
 * Version: v3 (2026-10-03) - v3: net_add_post() for TEST-build endpoints of other modules (hostwatch_ctl.c).
 *   v2: net_set_status() - app lines on the status page (the only view once USB is off)
 * Scope: PRODUCTION - anchor-box firmware, live on the box at the nav station since 2026-10-02 (installed by signed OTA).
 * Architecture: anchor-box firmware, network layer. ESP-IDF esp_wifi + esp_http_server + esp_ota_ops (signature
 *   checked by esp_ota_end with CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT) + espressif/mdns. Started by
 *   app_main.c; the laptop side is tools/ota_push.py.
 * Invocation: imported by app_main.c; net_start(gate) once after board_init(). Wi-Fi login from
 *   sdkconfig.secrets (CONFIG_ANCHORBOX_WIFI_*); empty SSID = Wi-Fi off.
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef bool (*net_ota_gate_t)(void);   /* returns true when an update may run now */

esp_err_t net_start(net_ota_gate_t gate);
void net_mark_healthy(void);            /* after a good start: cancel rollback of a fresh update */
void net_set_status(const char *text);   /* extra lines shown by GET / (clock, zone, night, monitor) */
/* TEST builds only: another module's POST endpoint on the same server; ESP_ERR_INVALID_STATE if no server */
esp_err_t net_add_post(const char *uri, esp_err_t (*handler)(httpd_req_t *));
