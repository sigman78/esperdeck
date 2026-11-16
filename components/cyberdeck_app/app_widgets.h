/*
 * app_widgets.h — shell-only widgets. cyberdeck_ui.h exposes the
 * reusable parts (ui_part.h) publicly. What stays here reads shell
 * state, or the shell composites it itself.
 */

#pragma once

#include "app_internal.h"

/** The StatusBar (docs/status-bar.md): full-width bar on its own
 *  background at row n-1 — a cluster of icon chips left, clock right, a
 *  live toast clipped into the gap between them. Implemented by
 *  app_statusbar.c; composited by the shell for NAV_CHROME_FULL screens. */
void ui_statusbar(uint64_t now);

/** The status tick, once per shell loop. It expires the toast and times
 *  the chip announcements. It invalidates the page when the row changes,
 *  so a still page shows the news. */
void status_tick(uint64_t now);

/** No announcement is live or waiting. The simulator's settle step reads it. */
bool status_idle(void);

/** Wi-Fi signal in dBm, 0 while the link is down. Cached for a second,
 *  because the driver call takes a lock. The StatusBar and the setup hub
 *  share it. */
int net_rssi(void);

/** "good", "fair" or "weak" for @p rssi, cut where the status chip cuts.
 *  @p pen receives its OVERLAY_COL_*. */
const char *rssi_word(int rssi, uint8_t *pen);

/** Wall-clock "HH:MM" once SNTP/host time exists; false until then. */
bool clock_str(char *buf, size_t sz);

/** Resolve one input event against the regions of the last frame
 *  (ui_hot.h). Touch events go by position, arrows and Enter by focus.
 *  Scroll events and other keys give UI_ACT_NONE. */
ui_act_t app_hot_input(const cyberdeck_input_t *ev, ui_key_t k);

/** A host as a tile: name, "user@host[:port]" in @p buf, accent stripe.
 *  HOME and the Setup > Hosts list draw it the same way. */
void host_tile(const conn_profile_t *p, ui_part_t *out, char *buf, size_t sz);

const char *wifi_status_str(void);
const char *ble_status_str(void);
