/**
 * Purpose: Runs the 'monitoring host down' watch on the box (ANC-25, Ship's Automation OQ-117 1b + 1c): polls the Pi's
 *   SignalK over Wi-Fi, checks the router before blaming the Pi, times the Cerbo's N2K frames, drives DO1 (panel
 *   AL-13 'Automation') and supplies the screen / status-page text. Inform only - it controls nothing.
 * Version: v1 (2026-10-03)
 * Scope: PRODUCTION - anchor-box firmware (live on the box at the nav station, installed by signed OTA).
 * Architecture: anchor-box firmware, control layer beside watch_ctl.c. Own low-priority task ("hostwatch", below
 *   the watch and the clock) - every blocking call (HTTP, ping, NVS) happens there, so the anchor watch loop is
 *   never delayed. Logic in hostwatch.c; N2K facts from n2k.c; DO1 via board.c; HTTP via esp_http_client; router
 *   check via esp_ping (ICMP). Nothing is ever sent to the Cerbo or on N2K.
 * Invocation: imported by app_main.c (hostwatch_ctl_start, after net_start - needs NVS + Wi-Fi), boatclock.c
 *   (status + clock notice) and ui_watch.c (notice).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

void hostwatch_ctl_start(void);
/* the line for the screens: "" when all is well, else e.g. "AUTOMATION: Pi / SignalK not answering" */
void hostwatch_ctl_notice(char *buf, size_t n);
/* status-page lines (no trailing newline) */
void hostwatch_ctl_status(char *buf, size_t n);
