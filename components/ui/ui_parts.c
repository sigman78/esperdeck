/*
 * ui_parts.c — small shared helpers: the drag converter, spinner and
 * noise glyphs, the profile accent and the session scrollbar. The slate
 * parts live in ui_part.c. No shell state.
 */

#include "cyberdeck_ui.h"

#include <stdio.h>

#ifndef CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT
#define CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT 100
#endif

int ui_drag_rows(ui_drag_t *d, int dy, int row_px)
{
    if (row_px <= 0) return 0;
    d->accum += dy * CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT;
    const int per_row = row_px * 100;
    int rows = d->accum / per_row;
    d->accum -= rows * per_row;
    return rows;
}

/* 8-frame braille spinner (U+2800 block — present in the font). */
uint16_t spinner_glyph(uint32_t frame)
{
    static const uint16_t sp[8] = {
        0x280B, 0x2819, 0x2839, 0x2838, 0x283C, 0x2834, 0x2826, 0x2827
    };
    return sp[frame % 8];
}

/* Braille "noise" glyph from a hash — the shared recipe for every static/
 * rain/decode effect. Skips the blank U+2800 pattern so a speck can never
 * be invisible. */
uint16_t braille_noise(uint32_t h)
{
    return (uint16_t)(0x2801 + h % 255u);
}

/* Stable per-profile accent from a djb2 hash of the name — profiles get a
 * visual identity on HOME and CONNECTING. RED (destructive) and WHITE are
 * deliberately excluded. */
uint8_t prof_accent(const char *name)
{
    static const uint8_t pal[] = {
        OVERLAY_COL_GREEN, OVERLAY_COL_CYAN, OVERLAY_COL_MAGENTA,
        OVERLAY_COL_AMBER, OVERLAY_COL_BLUE,
    };
    uint32_t h = 5381;
    while (*name) h = h * 33 + (uint8_t)*name++;
    return pal[h % (sizeof(pal) / sizeof(pal[0]))];
}

/* U+2581..U+2588 fill the bottom n/8 of a cell. Terminus has no
 * upper-eighth glyphs, so the fill anchors to the bottom. */
#define BLK_LOWER(n)  ((uint16_t)(0x2580u + (n)))

void draw_scrollbar(int offset, int total)
{
    if (total <= 0) return;

    /* Pin the pen; never inherit the caller's. TRACK bakes one look
     * for every accent, and the theme test locks that. */
    ui_pen(OVERLAY_COL_DEFAULT);

    const int col  = ui_cols() - 1;
    const int rows = ui_rows();
    if (col < 0 || rows < 1) return;

    if (offset < 0)     offset = 0;
    if (offset > total) offset = total;

    /* Fill height in eighths of a cell, measured up from the bottom. */
    int f = (int)(((long)offset * 8 * rows) / total);
    if (f > 8 * rows) f = 8 * rows;
    const int full = f / 8;
    const int rem  = f % 8;

    /* Every cell shares the one TRACK entry, so the column's background
     * is seamless and the fill always contrasts with it. */
    for (int y = 0; y < rows; y++) {
        const int from_bottom = rows - 1 - y;
        uint16_t  cp;
        if (from_bottom < full)                cp = UI_BLOCK;
        else if (from_bottom == full && rem)   cp = BLK_LOWER(rem);
        else                                   cp = ' ';
        ui_putch(col, y, cp, UI_TRACK);
    }

    /* Pen 0 BAR is neutral gray with dark text. Nothing at offset 0 —
     * the empty bar already says it. */
    if (offset > 0) {
        char label[16];
        int n  = snprintf(label, sizeof(label), " %d ", offset);
        int lx = col - 1 - n;
        if (lx >= 0)
            ui_puts(lx, 0, label, UI_BAR);
    }
}
