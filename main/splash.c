/*
 * splash.c — Colour-capability showcase splash screen and status helpers.
 *
 * Uses full ANSI escape sequences via vterm; the same tsm/display
 * pipeline used for SSH session output.
 *
 * Layout (100 × 30 terminal):
 *   Row  0     : blank
 *   Row  1     : title bar (full-width, bright cyan on dark blue)
 *   Row  2     : subtitle
 *   Row  3     : blank
 *   Row  4-5   : ANSI-16 FG colour strip
 *   Row  6-7   : ANSI-16 BG colour strip
 *   Row  8     : SGR attributes
 *   Row  9     : blank
 *   Row 10     : colour cube label
 *   Row 11-16  : 6 × 6×6 RGB cube rows
 *   Row 17     : blank
 *   Row 18     : grayscale label
 *   Row 19     : 24-step grayscale ramp
 *   Row 20     : blank
 *   Row 21+    : status / ready lines
 */

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include "vterm.h"
#include "splash.h"

/* -------------------------------------------------------------------------
 * ANSI helper macros — \e is a GCC extension (= ESC = 0x1B)
 * ---------------------------------------------------------------------- */
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
static void bg(int n) { vf("\e[48;5;%dm", n); }
static void fgbg(int f, int b) { vf("\e[38;5;%d;48;5;%dm", f, b); }

/* -------------------------------------------------------------------------
 * splash_show() — full colour-capability showcase
 * ---------------------------------------------------------------------- */
void splash_show(void)
{
    /* Clear screen, home cursor */
    vw(AC_CLS AC_HOME);

    /* ── Title bar (full width) ─────────────────────────────────────── */
    fgbg(14, 17);
    vw(AC_BOLD "  ╔");
    for (int i = 0; i < 94; i++) vw("═");
    vw("╗  \r\n");

    fgbg(14, 17); vw(AC_BOLD "  ║");
    fgbg(15, 17); vw(AC_BOLD
       "                CYBERDECK SSH TERMINAL v0.1"
       "          ESP32-S3 · Terminus 8×16                 ");
    fgbg(14, 17); vw(AC_BOLD "║  \r\n");

    fgbg(14, 17); vw(AC_BOLD "  ╚");
    for (int i = 0; i < 94; i++) vw("═");
    vw("╝  \r\n");
    vw(AC_RESET "\r\n");

    /* ── ANSI-16 foreground colours ─────────────────────────────────── */
    fgbg(7, 0);
    vw(AC_BOLD "  FG: " AC_RESET);
    for (int c = 0; c < 16; c++) {
        vf("\e[38;5;%dm", c);
        bg(c < 8 ? 8 : 0);
        vf(" %2d ", c);
    }
    vw(AC_RESET "\r\n");

    /* ── ANSI-16 background colours ─────────────────────────────────── */
    fgbg(7, 0);
    vw(AC_BOLD "  BG: " AC_RESET);
    for (int c = 0; c < 16; c++) {
        fg(c < 8 ? 15 : 0);
        vf("\e[48;5;%dm", c);
        vf(" %2d ", c);
    }
    vw(AC_RESET "\r\n\r\n");

    /* ── SGR attribute demo ──────────────────────────────────────────── */
    fgbg(7, 0);
    vw("  " AC_BOLD    "Bold"             AC_RESET
       "  " AC_UNDER   "Underline"        AC_RESET
       "  " AC_REV     "Reverse"          AC_RESET
       "  \e[1;32m"    "Bold+Green"       AC_RESET
       "  \e[4;33m"    "Underline+Yellow" AC_RESET
       "  \e[7;35m"    "Reverse+Magenta"  AC_RESET "\r\n\r\n");

    /* ── 6×6×6 colour cube (ANSI 16-231) ───────────────────────────── */
    fgbg(7, 0);
    vw(AC_BOLD "  256-color cube:" AC_RESET "\r\n");

    for (int r = 0; r < 6; r++) {
        vw("  ");
        for (int g = 0; g < 6; g++) {
            for (int b = 0; b < 6; b++) {
                int idx = 16 + r * 36 + g * 6 + b;
                int light = (r > 2 || g > 2 || (r + g + b) > 6);
                vf("\e[38;5;%d;48;5;%dm", light ? 0 : 15, idx);
                vw("  ");
            }
            if (g < 5) { fgbg(0, 0); vw(" "); }
        }
        vw(AC_RESET "\r\n");
    }
    vw("\r\n");

    /* ── Grayscale ramp (ANSI 232-255) ──────────────────────────────── */
    fgbg(7, 0);
    vw(AC_BOLD "  Grayscale:" AC_RESET "  ");
    for (int i = 0; i < 24; i++) {
        vf("\e[38;5;%d;48;5;%dm", (i < 12) ? 15 : 0, 232 + i);
        vf("%3d", 232 + i);
    }
    vw(AC_RESET "\r\n\r\n");

    /* ── Status line ─────────────────────────────────────────────────── */
    fgbg(10, 0); vw(AC_BOLD "  Initializing system..." AC_RESET "\r\n");
}

/* -------------------------------------------------------------------------
 * Coloured status helpers
 *   info  — cyan   [*]
 *   ok    — green  [✓]
 *   fail  — red    [✗]
 * ---------------------------------------------------------------------- */
void splash_status_info(const char *msg)
{
    fgbg(6, 0); vw(AC_BOLD "  [");
    fg(14); vw("*");
    fg(6);  vw("] " AC_RESET);
    fgbg(7, 0);
    vterm_write(msg, strlen(msg));
    vw(AC_RESET "\r\n");
}

void splash_status_ok(const char *msg)
{
    fgbg(2, 0); vw(AC_BOLD "  [");
    fg(10); vw("✓");
    fg(2);  vw("] " AC_RESET);
    fgbg(7, 0);
    vterm_write(msg, strlen(msg));
    vw(AC_RESET "\r\n");
}

void splash_status_fail(const char *msg)
{
    fgbg(1, 0); vw(AC_BOLD "  [");
    fg(9);  vw("✗");
    fg(1);  vw("] " AC_RESET);
    fgbg(7, 0);
    vterm_write(msg, strlen(msg));
    vw(AC_RESET "\r\n");
}
