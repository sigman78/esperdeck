/*
 * display_fx_internal.h — effect state shared between display_fx.c (owner)
 * and display_render.c (ISR consumer). NOT part of the public display API.
 *
 * Everything here lives in DRAM so the bounce-buffer ISR can read it
 * without touching flash cache.
 */

#ifndef DISPLAY_FX_INTERNAL_H
#define DISPLAY_FX_INTERNAL_H

#include "display_fx.h"
#include "esp_attr.h"

#define DISPLAY_FX_MAX_ROWS 32   /* >= DISPLAY_HEIGHT / FONT_HEIGHT (30) */

/* Active config (clamped copy; written by display_fx_set, read by ISR). */
extern DRAM_ATTR display_fx_cfg_t g_fx_cfg;

/* Frame counter — incremented by the renderer once per frame (band 0);
 * read cross-context by display_fx_touch_row. */
extern DRAM_ATTR volatile uint8_t g_fx_frame;

#if DISPLAY_FX_ROW_GLOW
/* Precomputed glow accent blend terms: [0] = (accent>>3)&0x18E3 (12.5%),
 * [1] = (accent>>2)&0x39E7 (25%). */
extern DRAM_ATTR uint16_t g_fx_glow_acc[2];

/* Per-row "last changed" stamps (g_fx_frame value at last flush of the row).
 * Written by display_fx_touch_row (task) and re-pinned by the renderer when
 * a row ages far out, so the uint8 counter can't wrap back into the glow
 * window. Byte stores from both contexts — volatile, races are benign. */
extern DRAM_ATTR volatile uint8_t g_fx_row_stamp[DISPLAY_FX_MAX_ROWS];
#endif

/* CRT line wobble: one full sine of horizontal offsets (±6 px max),
 * rebuilt by display_fx_set for the active amplitude. The ISR indexes it
 * with g_fx_frame directly — one sine cycle per 256-frame (~6.5 s) sweep
 * of the scanning bump. */
extern DRAM_ATTR int8_t g_fx_wobble_lut[256];

/* Event countdowns (frames remaining) + total durations captured at arm
 * time. Totals are written strictly before counters so the ISR never sees
 * a nonzero counter with a zero total. */
extern DRAM_ATTR volatile int16_t g_fx_wipe_left;
extern DRAM_ATTR volatile int16_t g_fx_wipe_total;
extern DRAM_ATTR volatile int16_t g_fx_collapse_left;
extern DRAM_ATTR volatile int16_t g_fx_collapse_total;
extern DRAM_ATTR volatile int16_t g_fx_static_left;

#endif /* DISPLAY_FX_INTERNAL_H */
