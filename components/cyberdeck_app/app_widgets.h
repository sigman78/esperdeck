/*
 * app_widgets.h — shell-only widgets. cyberdeck_ui.h exposes the
 * reusable parts (tile grid, list, buttons, chrome helpers) publicly.
 * What stays here reads shell state, or the shell composites it
 * itself.
 */

#pragma once

#include "app_internal.h"

/** The StatusBar (docs/status-bar.md): full-width bar on its own
 *  background at row n-1 — a cluster of icon chips left, clock right, a
 *  live toast clipped into the gap between them. Implemented by
 *  app_statusbar.c; composited by the shell for NAV_CHROME_FULL screens. */
void ui_statusbar(uint64_t now);

/** Free-RAM summary for the header (debug stat, dev screens only). */
void ram_stats(char *buf, size_t sz);

/** Wall-clock "HH:MM" once SNTP/host time exists; false until then. */
bool clock_str(char *buf, size_t sz);

/** Resolve one input event against the regions of the last frame
 *  (ui_hot.h). Touch events go by position, arrows and Enter by focus.
 *  Scroll events and other keys give UI_ACT_NONE. */
ui_act_t app_hot_input(const cyberdeck_input_t *ev, ui_key_t k);

const char *wifi_status_str(void);
const char *ble_status_str(void);
