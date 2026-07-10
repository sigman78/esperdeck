/*
 * color.c — ANSI-256 → RGB565 conversion
 *
 * SPDX-License-Identifier: MIT
 */
#include "color.h"

/* ── Named colors (indices 0–15) ─────────────────────────────────────────── */

/* Standard xterm/VT220 16-color palette as RGB565.
 * Matches the values used by display_ansi_to_rgb565() in display.c. */
static const uint16_t s_named[16] = {
    /* 0  black   */ 0x0000,
    /* 1  red     */ 0x8000,
    /* 2  green   */ 0x0400,
    /* 3  yellow  */ 0x8400,
    /* 4  blue    */ 0x0010,
    /* 5  magenta */ 0x8010,
    /* 6  cyan    */ 0x0410,
    /* 7  white   */ 0xBDF7,
    /* 8  br.blk  */ 0x4208,
    /* 9  br.red  */ 0xF800,
    /* 10 br.grn  */ 0x07E0,
    /* 11 br.yel  */ 0xFFE0,
    /* 12 br.blu  */ 0x001F,
    /* 13 br.mag  */ 0xF81F,
    /* 14 br.cyn  */ 0x07FF,
    /* 15 br.wht  */ 0xFFFF,
};

/* ── 6×6×6 colour cube helper ────────────────────────────────────────────── */

static inline uint8_t cube_to_8(int v)
{
    /* xterm cube: 0→0, 1→95, 2→135, 3→175, 4→215, 5→255 */
    return v ? (uint8_t)(55 + v * 40) : 0u;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

uint16_t color_ansi(uint8_t idx)
{
    if (idx < 16) {
        return s_named[idx];
    }
    if (idx < 232) {
        /* 6×6×6 cube: index 16–231 */
        int i = idx - 16;
        int b = i % 6;  i /= 6;
        int g = i % 6;  i /= 6;
        int r = i % 6;
        return color_rgb(cube_to_8(r), cube_to_8(g), cube_to_8(b));
    }
    /* Grayscale: indices 232–255 → 8, 18, 28 … 238 */
    uint8_t v = (uint8_t)(8 + (idx - 232) * 10);
    return color_rgb(v, v, v);
}
