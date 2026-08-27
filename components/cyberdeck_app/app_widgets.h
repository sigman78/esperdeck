/*
 * app_widgets.h — shell-only widgets. cyberdeck_ui.h exposes the
 * reusable parts (tile grid, list, buttons, chrome helpers) publicly.
 * What stays here reads shell state, or the shell composites it
 * itself.
 */

#pragma once

#include "app_internal.h"

/** The StatusBar (ui-spec, locked): full-width bar on its own background
 *  at row n-1 — lettered indicator patches (NET/KBD), clock right; a
 *  live toast takes the indicator span over. Composited by the shell
 *  for NAV_CHROME_FULL screens. */
void ui_statusbar(uint64_t now);

/* app_pacman.c — the HOME marquee (dynamic-sprite showcase).
 * draw_pacman renders one tick at overlay @p row. pacman_reset marks
 * its static pellet sprite for reload after session entry blanks the
 * slots. */
void draw_pacman(int row);
void pacman_reset(void);

/** Free-RAM summary for the header (debug stat, dev screens only). */
void ram_stats(char *buf, size_t sz);

const char *wifi_status_str(void);
const char *ble_status_str(void);
