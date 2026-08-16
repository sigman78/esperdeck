/*
 * splash.c — boot splash: an ANSI color-capability test card drawn through
 * the normal vterm pipeline. Layout adapts to the active character grid
 * (100×30 / 80×24 / 66×20); compiled out unless CONFIG_CYBERDECK_BOOT_SPLASH.
 */

#include "splash.h"

#if CONFIG_CYBERDECK_BOOT_SPLASH

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "vterm.h"
#include "display.h"
#include "font.h"
#include "esp_app_desc.h"

#define AC_RESET  "\e[0m"
#define AC_BOLD   "\e[1m"
#define AC_UNDER  "\e[4m"
#define AC_REV    "\e[7m"
#define AC_CLS    "\e[2J"
#define AC_HOME   "\e[H"

/* Write literal string via vterm */
#define vw(s)  vterm_write((s), sizeof(s) - 1)

/* Write formatted string via vterm */
static void vf(const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0)
        vterm_write(buf, (size_t)n);
}

static void fg(int n) { vf("\e[38;5;%dm", n); }
static void fgbg(int f, int b) { vf("\e[38;5;%d;48;5;%dm", f, b); }

void splash_show(void)
{
    const int  cols  = display_text_cols();
    const int  rows  = display_text_rows();
    const bool roomy = rows >= 24;          /* blanks + attr demo fit */

    vw(AC_CLS AC_HOME);

    /* Title box, full width minus a 2-cell margin. */
    const int inner = cols - 6;
    char title[96];
    snprintf(title, sizeof(title), "CYBERDECK SSH TERMINAL %s - ESP32-S3 - Terminus %s",
             esp_app_get_description()->version,
             font_size_name(font_active_size()));
    int tlen = (int)strlen(title);
    if (tlen > inner) { tlen = inner; title[tlen] = '\0'; }

    fgbg(14, 17);
    vw(AC_BOLD "  ╔");
    for (int i = 0; i < inner; i++) vw("═");
    vw("╗\r\n  ║");
    fg(15);
    vf("%*s%s%*s", (inner - tlen) / 2, "", title,
       inner - tlen - (inner - tlen) / 2, "");
    fg(14);
    vw("║\r\n  ╚");
    for (int i = 0; i < inner; i++) vw("═");
    vw("╝" AC_RESET "\r\n");
    if (roomy) vw("\r\n");

    /* ANSI-16 FG / BG strips; swatch width shrinks with the grid. */
    const int sw = cols >= 88 ? 4 : 3;
    fgbg(7, 0);
    vw(AC_BOLD "  FG: " AC_RESET);
    for (int c = 0; c < 16; c++) {
        fg(c);
        vf("\e[48;5;%dm", c < 8 ? 8 : 0);
        vf(sw == 4 ? " %2d " : "%2d ", c);
    }
    vw(AC_RESET "\r\n");
    fgbg(7, 0);
    vw(AC_BOLD "  BG: " AC_RESET);
    for (int c = 0; c < 16; c++) {
        fg(c < 8 ? 15 : 0);
        vf("\e[48;5;%dm", c);
        vf(sw == 4 ? " %2d " : "%2d ", c);
    }
    vw(AC_RESET "\r\n");

    /* SGR attribute demo — needs both spare rows and width. */
    if (roomy && cols >= 76) {
        vw("\r\n");
        fgbg(7, 0);
        vw("  " AC_BOLD    "Bold"             AC_RESET
           "  " AC_UNDER   "Underline"        AC_RESET
           "  " AC_REV     "Reverse"          AC_RESET
           "  \e[1;32m"    "Bold+Green"       AC_RESET
           "  \e[4;33m"    "Underline+Yellow" AC_RESET
           "  \e[7;35m"    "Reverse+Magenta"  AC_RESET "\r\n");
    }
    if (roomy) vw("\r\n");

    /* 6×6×6 colour cube (ANSI 16-231): 2-wide cells with green-block gaps
     * on wide grids, 1-wide packed on narrow ones. */
    const int cw = cols >= 80 ? 2 : 1;
    fgbg(7, 0);
    vw(AC_BOLD "  256-color cube:" AC_RESET "\r\n");
    for (int r = 0; r < 6; r++) {
        vw("  ");
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                int idx = 16 + r * 36 + g * 6 + b;
                vf("\e[48;5;%dm%s", idx, cw == 2 ? "  " : " ");
            }
            if (cw == 2 && g < 5) { fgbg(0, 0); vw(" "); }
        }
        vw(AC_RESET "\r\n");
    }
    if (roomy) vw("\r\n");

    /* Grayscale ramp (ANSI 232-255), numbered only when it fits. */
    const int gw = cols >= 88 ? 3 : 2;
    fgbg(7, 0);
    vw(AC_BOLD "  Gray: " AC_RESET);
    for (int i = 0; i < 24; i++) {
        vf("\e[38;5;%d;48;5;%dm", (i < 12) ? 15 : 0, 232 + i);
        if (gw == 3) vf("%3d", 232 + i);
        else         vw("  ");
    }
    vw(AC_RESET "\r\n");
    if (roomy) vw("\r\n");

    fgbg(10, 0); vw(AC_BOLD "  Initializing system..." AC_RESET "\r\n");
}

#endif /* CONFIG_CYBERDECK_BOOT_SPLASH */
