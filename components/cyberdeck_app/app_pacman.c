/*
 * app_pacman.c — Pac-Man marquee plugin, the dynamic-sprite-glyph
 * showcase riding the home_strip hook.
 *
 * Four sprite slots animate a TWO-CELL Pac-Man: a full-height disc, 2W x
 * 2W (16x16 at 8x16). He chomps across a row of pellets at PIXEL
 * resolution. At sub-cell offset k, his body straddles three cells,
 * sliced from one art row. TAIL is art >> (W+k). MID is (art >> k) & M.
 * HEAD is (art << (W-k)) & M, with the pellet composited in until the
 * mouth front crosses its center. Slot DOT is the static pellet placed
 * in every cell ahead; cells behind stay blank (eaten). When he exits
 * the right edge the whole run wraps and the pellets respawn.
 *
 * Sprite slots are a global resource — record every taker here until an
 * allocator exists. An offline generator predefines the art per font
 * size: uint32 rows, 2W bits wide, bit (2W-1) = leftmost. It embeds the
 * result directly.
 */

#include "app_internal.h"
#include "app_screens.h"     /* SCR_HOME — the strip's owner */
#include "cyberdeck_plugin.h"
#include "font.h"

enum { SPR_PAC_TAIL = 0, SPR_PAC_MID = 1, SPR_PAC_HEAD = 2, SPR_PAC_DOT = 3 };

static const uint32_t pac2_open_8[] = {
    0x0007E0, 0x001FF8, 0x003F38, 0x007F30, 0x007FE0, 0x00FFC0,
    0x00FF80, 0x00FF00, 0x00FF00, 0x00FF80, 0x00FFC0, 0x007FE0,
    0x007FF0, 0x003FF8, 0x001FF8, 0x0007E0 };
static const uint32_t pac2_shut_8[] = {
    0x0007E0, 0x001FF8, 0x003F3C, 0x007F3E, 0x007FFE, 0x00FFFF,
    0x00FFFF, 0x00FFFF, 0x00FFFF, 0x00FFFF, 0x00FFFF, 0x007FFE,
    0x007FFE, 0x003FFC, 0x001FF8, 0x0007E0 };
static const uint16_t pac_dot_8[]   = { 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x018, 0x018, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000 };

static const uint32_t pac2_open_10[] = {
    0x001F80, 0x007FE0, 0x01FFF8, 0x03FC70, 0x03FC60, 0x07FC40,
    0x07FF80, 0x0FFF00, 0x0FFE00, 0x0FFC00, 0x0FFC00, 0x0FFE00,
    0x0FFF00, 0x07FF80, 0x07FFC0, 0x03FFE0, 0x03FFF0, 0x01FFF8,
    0x007FE0, 0x001F80 };
static const uint32_t pac2_shut_10[] = {
    0x001F80, 0x007FE0, 0x01FFF8, 0x03FC7C, 0x03FC7C, 0x07FC7E,
    0x07FFFE, 0x0FFFFF, 0x0FFFFF, 0x0FFFFF, 0x0FFFFF, 0x0FFFFF,
    0x0FFFFF, 0x07FFFE, 0x07FFFE, 0x03FFFC, 0x03FFFC, 0x01FFF8,
    0x007FE0, 0x001F80 };
static const uint16_t pac_dot_10[]  = { 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x030, 0x030, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000 };

static const uint32_t pac2_open_12[] = {
    0x00FF00, 0x03FFC0, 0x07FFE0, 0x1FFFF0, 0x1FF1E0, 0x3FF1C0,
    0x7FF180, 0x7FFF00, 0xFFFE00, 0xFFFC00, 0xFFF800, 0xFFF000,
    0xFFF000, 0xFFF800, 0xFFFC00, 0xFFFE00, 0x7FFF00, 0x7FFF80,
    0x3FFFC0, 0x1FFFE0, 0x1FFFF0, 0x07FFE0, 0x03FFC0, 0x00FF00 };
static const uint32_t pac2_shut_12[] = {
    0x00FF00, 0x03FFC0, 0x07FFE0, 0x1FFFF8, 0x1FF1F8, 0x3FF1FC,
    0x7FF1FE, 0x7FFFFE, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF,
    0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0x7FFFFE, 0x7FFFFE,
    0x3FFFFC, 0x1FFFF8, 0x1FFFF8, 0x07FFE0, 0x03FFC0, 0x00FF00 };
static const uint16_t pac_dot_12[]  = { 0x000, 0x000, 0x000, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x0E0, 0x0E0, 0x0E0, 0x000, 0x000,
    0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000, 0x000 };

/* The strip draw (re)loads the static pellet on marquee (re)entry, not
 * per tick. A per-tick load would invalidate its glyph-cache line. It
 * would also force the ISR to re-decode the most-shared sprite on
 * screen, every frame, for nothing. */
static bool s_pac_dot_ready;

/* Away from HOME the sprite slots are fair game (session entry blanks
 * them all) — mark the pellet for reload on the next draw. */
static void pac_tick(uint64_t now)
{
    (void)now;
    if (nav_current() != SCR_HOME)
        s_pac_dot_ready = false;
}

static const uint16_t *pac_dot_art(void)
{
    switch (font_width()) {
    case 10: return pac_dot_10;
    case 12: return pac_dot_12;
    default: return pac_dot_8;
    }
}

static void pac_strip(int row, uint64_t now)
{
    (void)now;
    const int W = font_width(), H = font_height(), cols = ui_cols();
    const uint16_t *dot = pac_dot_art();
    const uint32_t *art;
    switch (W) {
    case 10: art = (app.anim_frame & 2) ? pac2_shut_10 : pac2_open_10; break;
    case 12: art = (app.anim_frame & 2) ? pac2_shut_12 : pac2_open_12; break;
    default: art = (app.anim_frame & 2) ? pac2_shut_8  : pac2_open_8;  break;
    }

    if (!s_pac_dot_ready) {
        font_sprite_set_rows16(SPR_PAC_DOT, dot);
        s_pac_dot_ready = true;
    }

    /* Body left edge in strip pixels; +2W of runout so the two-cell disc
     * fully exits before the wrap. */
    const int span = (cols + 2) * W;
    const int px   = (int)((app.anim_frame * 4u) % (unsigned)span);
    const int ci   = px / W, k = px % W;
    const unsigned wmask = (1u << W) - 1u;
    /* The pellet ahead survives until the mouth front crosses its center. */
    const bool dot_alive = k <= W / 2;

    /* Sized in ROWS: glyph_bytes >= height at every size (uint16 per row
     * regardless of the packed byte width — a /2 here overflowed the stack
     * on 8x16-only builds, where rows pack to single bytes). */
    uint16_t t[FONT_MAX_GLYPH_BYTES];
    for (int r = 0; r < H; r++)
        t[r] = (uint16_t)(art[r] >> (W + k));
    /* Sprites update before ui_present() publishes the matching cell
     * placement. An ISR frame landing in between can therefore pair new
     * art with last tick's cells. This gives one LCD frame of ghosting
     * on cell-advance ticks. It self-heals; this is an accepted
     * tradeoff. */
    font_sprite_set_rows16(SPR_PAC_TAIL, t);
    for (int r = 0; r < H; r++)
        t[r] = (uint16_t)((art[r] >> k) & wmask);
    font_sprite_set_rows16(SPR_PAC_MID, t);
    if (k) {
        for (int r = 0; r < H; r++) {
            unsigned v = (unsigned)(art[r] << (W - k)) & wmask;
            if (dot_alive)
                v |= dot[r];
            t[r] = (uint16_t)v;
        }
        font_sprite_set_rows16(SPR_PAC_HEAD, t);
    }

    ui_pen(OVERLAY_COL_AMBER);
    if (ci < cols)
        ui_putch(ci, row, font_sprite_cp(SPR_PAC_TAIL), 0);
    if (ci + 1 < cols)
        ui_putch(ci + 1, row, font_sprite_cp(SPR_PAC_MID), 0);
    if (k && ci + 2 < cols)
        ui_putch(ci + 2, row, font_sprite_cp(SPR_PAC_HEAD), 0);
    ui_pen(OVERLAY_COL_BLUE);
    for (int x = ci + (k ? 3 : 2); x < cols; x++)
        ui_putch(x, row, font_sprite_cp(SPR_PAC_DOT), 0);
    ui_pen(OVERLAY_COL_DEFAULT);
}

const cyberdeck_plugin_t pacman_plugin = {
    .name       = "pacman",
    .tick       = pac_tick,
    .home_strip = pac_strip,
};
