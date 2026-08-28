/*
 * app_saver.c — idle screensaver plugin: braille digital rain painting
 * the wall clock. Pure eye-candy on a real screen. The plugin tick
 * pushes SCR_SAVER when session_guard reports the deck idle on HOME
 * or the gate pad. Any input pops it. The lock policy lives in
 * session_guard.c (extensibility item 7).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "cyberdeck_plugin.h"
#include "session_guard.h"
#include "keystore.h"

#include <string.h>

/* Input inside this window after the rain went up still acts on the
 * screen underneath — a keypress aimed at a HOME visible milliseconds
 * ago must not vanish into a wake. */
#define SAVER_WAKE_GRACE_MS  1000

static uint64_t s_since;             /* when the rain went up */

/* Bold 6x7 block font (2-px strokes) for the screensaver clock, bit 5 =
 * leftmost column. Digits 0-9 plus ':' at index 10 (blinked by clk_mask). */
static const uint8_t k_clk_font[11][7] = {
    { 0x1E, 0x33, 0x33, 0x33, 0x33, 0x33, 0x1E },   /* 0 */
    { 0x0C, 0x1C, 0x0C, 0x0C, 0x0C, 0x0C, 0x3F },   /* 1 */
    { 0x1E, 0x33, 0x03, 0x06, 0x0C, 0x18, 0x3F },   /* 2 */
    { 0x3F, 0x06, 0x0C, 0x06, 0x03, 0x33, 0x1E },   /* 3 */
    { 0x06, 0x0E, 0x1A, 0x36, 0x3F, 0x06, 0x06 },   /* 4 */
    { 0x3F, 0x30, 0x3E, 0x03, 0x03, 0x33, 0x1E },   /* 5 */
    { 0x1E, 0x30, 0x3E, 0x33, 0x33, 0x33, 0x1E },   /* 6 */
    { 0x3F, 0x03, 0x06, 0x0C, 0x18, 0x18, 0x18 },   /* 7 */
    { 0x1E, 0x33, 0x33, 0x1E, 0x33, 0x33, 0x1E },   /* 8 */
    { 0x1E, 0x33, 0x33, 0x1F, 0x03, 0x03, 0x1E },   /* 9 */
    { 0x00, 0x0C, 0x0C, 0x00, 0x0C, 0x0C, 0x00 },   /* : */
};

/* One font pixel = 2x2 cells: "HH:MM" spans 544x224 px on the 800x480 panel. */
#define CLK_FW   6                           /* font pixels per glyph row   */
#define CLK_SX   2
#define CLK_SY   2
#define CLK_ADV  ((CLK_FW + 1) * CLK_SX)     /* char advance incl. 1-px gap */
#define CLK_W    (5 * CLK_ADV - CLK_SX)      /* "HH:MM" = 68 cells wide     */
#define CLK_H    (7 * CLK_SY)                /* 14 cells tall               */
#define CLK_CYCLE 300                        /* frames per paint/wash cycle */
#define CLK_WASH  100                        /* of which: rain washes off   */

typedef struct {
    int     ox, oy;        /* top-left cell of the clock block  */
    uint8_t glyph[5];      /* k_clk_font indices for "HH:MM"    */
    bool    colon_on;      /* blink phase; masks the ':' glyph  */
} clk_geom_t;

/* Is cell (x,y) inside a lit font pixel of the big clock? */
static bool clk_mask(const clk_geom_t *g, int x, int y)
{
    int dx = x - g->ox, dy = y - g->oy;
    if (dx < 0 || dx >= CLK_W || dy < 0 || dy >= CLK_H) return false;
    int ci = dx / CLK_ADV, cx = dx % CLK_ADV;
    if (cx >= CLK_FW * CLK_SX) return false;          /* inter-char gap */
    uint8_t gl = g->glyph[ci];
    if (gl == 10 && !g->colon_on) return false;       /* colon blink    */
    return (k_clk_font[gl][dy / CLK_SY] >> (CLK_FW - 1 - cx / CLK_SX)) & 1;
}

/* Rain paints the clock out of nothing. Digits start as black
 * rain-holes. Each head that strikes a stroke settles as a wet blue
 * cell (~10 s to paint). In the cycle's last 10 s, the rain scrubs it
 * back off. The block then hops to a fresh spot. Nothing stays put
 * longer than 30 s (LCD safety). It falls back to a floating chip
 * until SNTP delivers real time. */
static void saver_render(uint64_t now)
{
    (void)now;
    static uint8_t head[100];    /* per-column head row (grid is 100 wide) */
    static bool    seeded = false;
    static uint8_t sp_ttl[100];  /* splash frames left per column          */
    static uint8_t sp_row[100];  /* row the splash plays on (above stroke) */
    static uint8_t glow[CLK_H][CLK_W];   /* per-cell rain-touch afterglow  */
    static clk_geom_t ck;
    static uint32_t hop_seen = UINT32_MAX;
    int W = ui_cols() > 100 ? 100 : ui_cols();
    int H = ui_rows();
    if (!seeded) {
        seeded = true;
        for (int c = 0; c < W; c++)
            head[c] = (uint8_t)((c * 37u + 11u) % (unsigned)H);
    }

    char clk[8];
    bool have_clk = clock_str(clk, sizeof(clk));
    bool big = have_clk && ui_cols() >= CLK_W + 4 && H >= CLK_H + 4;
    if (big) {
        /* Hop to a fresh spot every 30 s (2-cell margin all around).
         * hop+1: cycle 0 must hash like any other, not pin the corner. */
        uint32_t hop = app.anim_frame / CLK_CYCLE;
        if (hop != hop_seen) {
            hop_seen = hop;
            uint32_t h = (hop + 1) * 2654435761u;
            ck.ox = 2 + (int)(h % (uint32_t)(ui_cols() - CLK_W - 3));
            ck.oy = 2 + (int)((h >> 10) % (uint32_t)(H - CLK_H - 3));
            memset(sp_ttl, 0, sizeof(sp_ttl));  /* splashes died with the move */
            memset(glow, 0, sizeof(glow));      /* ...and so did the wet spots */
        }
        for (int i = 0; i < 5; i++)
            ck.glyph[i] = clk[i] == ':' ? 10 : (uint8_t)(clk[i] - '0');
        ck.colon_on = ((app.anim_frame / 5) & 1) == 0;      /* 1 Hz blink */
    }

    /* Wash-off window: ten seconds is enough for every head to visit
     * every row. Slowest columns wrap in 9 s, so the block is bare by
     * the hop. */
    bool washing = big && app.anim_frame % CLK_CYCLE >= CLK_CYCLE - CLK_WASH;

    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    for (int c = 0; c < W; c++) {
        if (app.anim_frame % ((c % 3) + 1) == 0) {     /* three fall speeds */
            head[c] = (uint8_t)((head[c] + 1) % (unsigned)H);
            /* Falling onto the top edge of a stroke throws up a splash —
             * not on a scrubbed-bare stroke, where rain just falls through. */
            if (big && head[c] > 0 && sp_ttl[c] == 0
                && clk_mask(&ck, c, head[c])
                && !clk_mask(&ck, c, head[c] - 1)
                && (!washing || glow[head[c] - ck.oy][c - ck.ox])) {
                sp_ttl[c] = 3;
                sp_row[c] = (uint8_t)(head[c] - 1);
            }
        }
        for (int k = 0; k < 6; k++) {                /* head + 5-cell trail */
            int y = (head[c] - k + H) % H;
            if (big && clk_mask(&ck, c, y)) {
                uint8_t *gl = &glow[y - ck.oy][c - ck.ox];
                if (!washing) {          /* rain paints the numerals...   */
                    if (k == 0)          /* a head glints, then stays wet */
                        *gl = 4;
                    continue;
                }
                if (k == 0) *gl = 0;     /* ...then scrubs them off again */
                if (*gl) continue;       /* remaining paint occludes rain */
            }
            ui_pen(k == 0 ? OVERLAY_COL_WHITE
                 : k <= 2 ? OVERLAY_COL_GREEN : OVERLAY_COL_BLUE);
            ui_putch(c, y, braille_noise((uint32_t)c * 31u
                                         + (uint32_t)y * 17u
                                         + (app.anim_frame >> 1)), 0);
        }
    }

    /* Painted cells: a struck cell glints (glow 4..2), then settles at 1 —
     * a wet blue pixel that stays lit until a washing head scrubs it. */
    for (int y = 0; big && y < CLK_H; y++) {
        for (int x = 0; x < CLK_W; x++) {
            uint8_t g = glow[y][x];
            if (!g || !clk_mask(&ck, ck.ox + x, ck.oy + y)) continue;
            if (g > 1) glow[y][x]--;
            ui_pen(OVERLAY_COL_BLUE);
            ui_putch(ck.ox + x, ck.oy + y,
                     g >= 3 ? UI_SHADE2 : UI_SHADE1, 0);
        }
    }

    /* Splashes draw over the rain: impact, bounce, sideways droplets. */
    for (int c = 0; big && c < W; c++) {
        if (!sp_ttl[c]) continue;
        int r = sp_row[c];
        switch (sp_ttl[c]--) {
        /* Re-check the mask at draw time. A colon blink or minute
         * rollover mid-splash can move a stroke under the cell; skip
         * the frame when that happens. */
        case 3:
            ui_pen(OVERLAY_COL_WHITE);
            if (!clk_mask(&ck, c, r)) ui_putch(c, r, 0x28C0, 0);   /* impact */
            break;
        case 2:
            ui_pen(OVERLAY_COL_GREEN);
            if (!clk_mask(&ck, c, r)) ui_putch(c, r, 0x2812, 0);   /* bounce */
            break;
        default:
            ui_pen(OVERLAY_COL_BLUE);                      /* scatter */
            if (c > 0     && !clk_mask(&ck, c - 1, r)) ui_putch(c - 1, r, 0x2840, 0);
            if (c + 1 < W && !clk_mask(&ck, c + 1, r)) ui_putch(c + 1, r, 0x2880, 0);
            break;
        }
    }

    /* Grid too small for the block: the time floats through the rain as a
     * chip, hopping every 10 s — still useful, still no fixed pixels. */
    if (have_clk && !big) {
        uint32_t h = (app.anim_frame / 100) * 2654435761u;
        int cx = 2 + (int)(h % (uint32_t)(ui_cols() - 12));
        int cy = 1 + (int)((h >> 10) % (uint32_t)(ui_rows() - 2));
        ui_pen(OVERLAY_COL_WHITE);
        ui_chip(cx, cy, 0, clk, 0, 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

static void saver_enter(intptr_t arg, uint64_t now)
{
    (void)arg;
    s_since = now;
}

static void saver_tick(uint64_t now)
{
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

/* Wake. Inside the grace the input also acts on the revealed screen.
 * With a locked store a real wake lands on the non-skippable device
 * pad, never on what the rain covered. The store went cold when the
 * deck idled (session_guard). */
static void saver_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                        uint64_t now)
{
    if (app.toast[0] && now >= app.toast_until) app.toast[0] = '\0';
    bool swallow = now - s_since >= SAVER_WAKE_GRACE_MS;
    if (swallow && keystore_state() == KEYSTORE_LOCKED) {
        unlock_open_gate(now);
        return;
    }
    nav_pop(now);
    if (!swallow)
        nav_dispatch_input(ev, k, ch, now);
}

const nav_screen_t saver_screen = {
    .name = "saver", .enter = saver_enter, .tick = saver_tick,
    .input = saver_input, .render = saver_render,
    .chrome = NAV_CHROME_NONE,
};

/* Engage: the rain covers an idle HOME, and the idle DEVICE gate pad —
 * that pad is non-skippable and must not sit lit forever; wake routes
 * straight back onto it. */
static void saver_plugin_tick(uint64_t now)
{
    int cur = nav_current();
    bool eligible = cur == SCR_HOME ||
                    (cur == SCR_UNLOCK && unlock_is_gate());
    if (eligible && session_guard_idle(now))
        nav_push(SCR_SAVER, 0, now);
}

const cyberdeck_plugin_t saver_plugin = {
    .name = "saver",
    .tick = saver_plugin_tick,
};
