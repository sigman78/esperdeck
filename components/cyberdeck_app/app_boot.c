/*
 * app_boot.c — the boot splash (ST_BOOT): a block CYBER*DECK logo wipes in
 * while WiFi/BLE come up; one tagline per boot.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "display_fx.h"
#include "keystore.h"

#include <stdio.h>
#include <string.h>

#ifndef BUILD_SIMULATOR
#include "esp_attr.h"   /* RTC_NOINIT_ATTR */
#endif

static struct {
    uint64_t until;                     /* when the splash ends */
} s_boot;

/* 5x5 block glyphs for the boot logo (row-major, '#' = filled). */
static const char *boot_glyph(char c)
{
    switch (c) {
    case 'C': return "#####" "#    " "#    " "#    " "#####";
    case 'Y': return "#   #" " # # " "  #  " "  #  " "  #  ";
    case 'B': return "#### " "#   #" "#### " "#   #" "#### ";
    case 'E': return "#####" "#    " "###  " "#    " "#####";
    case 'R': return "#### " "#   #" "#### " "#  # " "#   #";
    case 'D': return "#### " "#   #" "#   #" "#   #" "#### ";
    case 'K': return "#   #" "#  # " "###  " "#  # " "#   #";
    case '*': return "  #  " "# # #" " ### " "# # #" "  #  ";
    case '+': return "# # #" " ### " "#####" " ### " "# # #";  /* twinkle */
    default:  return "     " "     " "     " "     " "     ";
    }
}

/* Boot counter in RTC memory: survives soft resets and starts random at
 * power-on, so boots walk the taglines without NVS or an RNG this early. */
#ifndef BUILD_SIMULATOR
static RTC_NOINIT_ATTR uint32_t s_boot_seq;
#else
static uint32_t s_boot_seq;
#endif

static const char *const BOOT_TAGLINES[] = {
    "SPINNING UP THE ICE",
    "WAKING THE WETWARE",
    "COLD BOOT, WARM HEART",
    "DIALING THE GRID",
    "CHECKING FOR BLACK ICE",
    "INITIALIZING",
};
#define TAGLINE_COUNT (sizeof(BOOT_TAGLINES) / sizeof(BOOT_TAGLINES[0]))

/* Logo wipes in left→right over ~80% of the boot delay behind a bright
 * white scan edge, then holds with the * twinkling. */
static void render_boot(uint64_t now)
{
    static const char LOGO[] = "CYBER*DECK";
    const int GW = 5, GH = 5, GAP = 1;
    int n = (int)strlen(LOGO);
    int total_w = n * (GW + GAP) - GAP;
    int x0 = (ui_cols() - total_w) / 2;
    int y0 = ui_rows() / 4;

    uint64_t start     = s_boot.until - app.cfg.boot_delay_ms;
    uint32_t reveal_ms = app.cfg.boot_delay_ms * 4 / 5;
    uint32_t el        = (uint32_t)(now - start);
    int reveal = reveal_ms ? (int)((uint64_t)el * total_w / reveal_ms) : total_w;
    if (reveal > total_w) reveal = total_w;

    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    bool done = (reveal == total_w);
    for (int i = 0; i < n; i++) {
        char ch = LOGO[i];
        /* Landed * twinkles: star/X-burst swap every ~0.8 s, white flash on
         * the swap frame. */
        bool star = (ch == '*');
        if (star && done && ((app.anim_frame >> 3) & 1)) ch = '+';
        bool flash = star && done && (app.anim_frame & 7) == 0;
        const char *g = boot_glyph(ch);
        int gx = x0 + i * (GW + GAP);
        uint8_t base = star ? (flash ? OVERLAY_COL_WHITE : OVERLAY_COL_MAGENTA)
                            : OVERLAY_COL_CYAN;
        for (int r = 0; r < GH; r++)
            for (int c = 0; c < GW; c++) {
                int col_abs = gx + c - x0;
                if (col_abs >= reveal || g[r * GW + c] != '#') continue;
                bool edge = !done && (col_abs >= reveal - 2); /* scan front */
                ui_pen(edge ? OVERLAY_COL_WHITE : base);
                ui_putch(gx + c, y0 + r, UI_BLOCK, 0);
            }
    }

    ui_pen(OVERLAY_COL_GREEN);
    const char *tag = BOOT_TAGLINES[s_boot_seq % TAGLINE_COUNT];
    char sub[40];
    snprintf(sub, sizeof(sub), "%s%.*s", tag, (int)(app.anim_frame % 4), "...");
    /* Fixed anchor: the full-dots form's width (dot count varies per frame). */
    ui_puts((ui_cols() - ((int)strlen(tag) + 3)) / 2, y0 + GH + 2, sub, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
}

static void boot_enter(intptr_t arg, uint64_t now)
{
    (void)arg;
    s_boot_seq++;   /* next tagline (RTC-resident, see decl above) */
    s_boot.until = now + app.cfg.boot_delay_ms;
}

/* Splash over (elapsed or skipped): HOME — behind the DEVICE gate whenever
 * a keystore exists (two-gates model: a store on the deck means the deck
 * is locked; no store = feature off, straight to HOME). */
static void boot_done(uint64_t now)
{
    display_fx_wipe();   /* raster-reveal whatever comes up next */
    if (keystore_state() == KEYSTORE_LOCKED)
        unlock_open_gate(now);
    else
        enter_home(now);
}

static void boot_tick(uint64_t now)
{
    if (now >= s_boot.until) {
        boot_done(now);
        return;
    }
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

static void boot_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                       uint64_t now)
{
    (void)ch;
    /* Any key OR touch skips the splash — touch-only decks have no
     * keyboard yet. */
    if (k != K_NONE || ev->type == CYBERDECK_INPUT_TAP ||
        ev->type == CYBERDECK_INPUT_LONG_PRESS)
        boot_done(now);
}

const nav_screen_t boot_screen = {
    .name = "boot", .enter = boot_enter, .tick = boot_tick,
    .input = boot_input, .render = render_boot, .chrome = NAV_CHROME_NONE,
};
