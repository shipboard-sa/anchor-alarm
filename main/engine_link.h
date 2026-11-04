/**
 * Purpose: The box's ONE link to the Ship's Automation rules engine (:8200) - one lowest-priority task and one reused
 *   keep-alive HTTP client that serve night watch (/night-watch), engine alerts (/alerts, /ack) and tank fills (/fill)
 *   in turn (ANQ-33 memory clean-up, 2026-10-09). Replaces the three tasks / three clients each opening and closing a
 *   connection every few seconds.
 * Version: v5 (2026-10-21) - v5: also serves the watchkeeper check-in (/watch, wkeep_ctl.c, ANQ-27). v4 (2026-10-18) - v4: also serves the cold-box EXPECTED? question (/restock, restock_ctl.c, ANC-45). v3 (2026-10-15) - v3: also serves the DOCK / LEAVE taps (/mode, boatmode_ctl.c, ANC-44). v2 (2026-10-11) - v2: also serves the HOT WATER tap (/hot-water, hotwater_ctl.c, ANC-43). v1 (2026-10-09)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, network layer under nightwatch_ctl.c / alerts_ctl.c / fill_ctl.c. Task "engine"
 *   (priority 1, stack in PSRAM) loops: each feature's service step does its pending tap or its due poll and says when
 *   it next needs the link; the task sleeps until the earliest of those or a kick (a tap). Requests run one at a time,
 *   so one 2 KB reply buffer (PSRAM) serves all three. The monitoring-host watch (hostwatch_ctl.c, the Pi's SignalK
 *   :3000) deliberately stays on its own task and client - the watchdog must not share fate with what it watches.
 * Invocation: imported by app_main.c (engine_link_start, after net_start and the three *_ctl_init) and by the three
 *   *_ctl.c (engine_link_request inside their service step, engine_link_kick from a tap).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ENGINE_BODY_MAX 2048          /* largest reply: /alerts at max=4 (< 1.8 KB, anchor-box-alerts.md) */

/* Start the engine task (no-op without Wi-Fi). The feature modules must be initialised first. */
void engine_link_start(void);

/* A tap is waiting: wake the engine task now (any task). */
void engine_link_kick(void);

/* Engine task only (inside a service step): one request on the shared client. path e.g. "/fill"; body NULL = GET,
 * else POST with Content-Type JSON; auth = add "Authorization: Bearer <box token>". The reply body lands in the
 * shared buffer (*reply, NUL-terminated, cut at ENGINE_BODY_MAX - 1) - valid until the next request.
 * Returns the HTTP status, 0 = no answer. A dropped keep-alive connection is retried once on a fresh one. */
int engine_link_request(const char *path, const char *body, bool auth, int timeout_ms, const char **reply);

/* Feature service steps, called by the engine task in turn: do a pending tap or a due poll (at most one request
 * each), return when this feature next needs the link (esp_timer microseconds; a pending tap -> now). */
int64_t nightwatch_ctl_service(int64_t now_us);
int64_t alerts_ctl_service(int64_t now_us);
int64_t fill_ctl_service(int64_t now_us);
int64_t hotwater_ctl_service(int64_t now_us);
int64_t boatmode_ctl_service(int64_t now_us);
int64_t restock_ctl_service(int64_t now_us);
int64_t wkeep_ctl_service(int64_t now_us);
