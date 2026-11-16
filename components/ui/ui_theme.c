/*
 * ui_theme.c — bakes the overlay style table. ui_theme_build() resolves
 * every look the rendition uses, once per ui_colors() call, not per cell
 * in the ISR. Tones and derivations carried over verbatim from the
 * retired per-cell resolver (docs/overlay-style.md).
 */

#include "ui_theme.h"

/* Accent tones. TEXT accents are VGA bright; bars use muted companions
 * of the same hues; wells their ~60% versions (ui-spec value wells). */
static const color_t ACCENT_TEXT[OVERLAY_ACCENTS] = {
    0,                        /* 0: default → the screen fg        */
    RGB565( 85, 255,  85),    /* 1 green   (VGA bright green)   */
    RGB565( 85, 255, 255),    /* 2 cyan    (VGA bright cyan)    */
    RGB565(255,  85, 255),    /* 3 magenta (VGA bright magenta) */
    RGB565(255, 255,  85),    /* 4 amber   (VGA yellow)         */
    RGB565(255,  85,  85),    /* 5 red     (VGA bright red)     */
    RGB565( 85,  85, 255),    /* 6 blue    (VGA bright blue)    */
    RGB565(255, 255, 255),    /* 7 white   (VGA white)          */
};
static const color_t ACCENT_BAR[OVERLAY_ACCENTS] = {
    RGB565(148, 148, 148),    /* 0 default → neutral gray       */
    RGB565( 96, 168,  96),    /* 1 sage                         */
    RGB565( 80, 160, 168),    /* 2 teal                         */
    RGB565(168,  96, 160),    /* 3 mauve                        */
    RGB565(200, 152,  72),    /* 4 ochre                        */
    RGB565(184,  88,  80),    /* 5 terracotta                   */
    RGB565(104, 112, 192),    /* 6 periwinkle                   */
    RGB565(255, 255, 255),    /* 7 white (kept pure — QR)       */
};
static const color_t ACCENT_WELL[OVERLAY_ACCENTS] = {
    RGB565( 89,  89,  89),    /* 0 gray                         */
    RGB565( 58, 101,  58),    /* 1 sage                         */
    RGB565( 48,  96, 101),    /* 2 teal                         */
    RGB565(101,  58,  96),    /* 3 mauve                        */
    RGB565(120,  91,  43),    /* 4 ochre                        */
    RGB565(110,  53,  48),    /* 5 terracotta                   */
    RGB565( 62,  67, 115),    /* 6 periwinkle                   */
    RGB565(153, 153, 153),    /* 7 white → mid gray             */
};

/* Pale tint of a bar hue — readable text ON that bar. Gray (0) and
 * white (7) keep dark text instead: QR modules stay dark-on-white. */
static color_t bar_text(int a, color_t bar, color_t screen_bg)
{
    if (a >= 1 && a <= 6)
        return (color_t)(((bar >> 2) & 0x39E7) + 0x7BEF + 0x39E7);
    return screen_bg;
}

/* Panel: the theme background lifted a fifth of the way to a cool
 * white — a neutral dark surface that still reads on a colored theme. */
static color_t panel_of(color_t bg)
{
    const int r = (bg >> 11) & 0x1F, g = (bg >> 5) & 0x3F, b = bg & 0x1F;
    return (color_t)(((r + (24 - r) / 5) << 11) |
                     ((g + (52 - g) / 5) << 5) |
                      (b + (30 - b) / 5));
}

/* @p n eighths of @p a over the rest of @p b, per channel. */
static color_t mix8(color_t a, color_t b, int n)
{
    const int r = (((a >> 11) & 0x1F) * n + ((b >> 11) & 0x1F) * (8 - n)) / 8;
    const int g = (((a >> 5) & 0x3F) * n + ((b >> 5) & 0x3F) * (8 - n)) / 8;
    const int bl = ((a & 0x1F) * n + (b & 0x1F) * (8 - n)) / 8;
    return (color_t)((r << 11) | (g << 5) | bl);
}

/* Same luma recipe as the display's mono effect: 0..62. */
static int luma(color_t p)
{
    const int r6 = ((p >> 11) & 0x1F) << 1;
    const int g6 = (p >> 5) & 0x3F;
    const int b6 = (p & 0x1F) << 1;
    return (r6 * 5 + g6 * 9 + b6 * 2) >> 4;
}

/* Muted text on a panel: five eighths of the text colour. A hue near
 * the panel's own brightness takes more, so it stays readable. */
static color_t panel_muted(color_t text, color_t panel)
{
    for (int n = 5; n < 8; n++) {
        const color_t c = mix8(text, panel, n);
        const int d = luma(c) - luma(panel);
        if (d >= 6 || d <= -6) return c;
    }
    return text;
}

/* Focus wash: 50% toward white (carry-safe half-sum). */
static color_t wash(color_t c)
{
    return (color_t)(((c >> 1) & 0x7BEF) + 0x7BEF);
}

void ui_theme_build(color_t fg, color_t bg, display_overlay_style_t *out)
{
    const color_t panel = panel_of(bg);

    for (int a = 0; a < OVERLAY_ACCENTS; a++) {
        const color_t text = a ? ACCENT_TEXT[a] : fg;

        out[UI_TEXT  * OVERLAY_ACCENTS + a] =
            (display_overlay_style_t){ text, bg };
        /* Muted = halfway between the accent and the theme bg (carry-safe
         * average). On the black theme this is plain half brightness.
         * On a colored theme it recedes into THAT color, not into black.
         * Halving toward black converges with a dark bg (theme test). */
        out[UI_MUTED * OVERLAY_ACCENTS + a] = (display_overlay_style_t){
            (color_t)(((text >> 1) & 0x7BEF) + ((bg >> 1) & 0x7BEF)), bg };
        out[UI_BAR   * OVERLAY_ACCENTS + a] = (display_overlay_style_t){
            bar_text(a, ACCENT_BAR[a], bg),  ACCENT_BAR[a] };
        out[UI_WELL  * OVERLAY_ACCENTS + a] = (display_overlay_style_t){
            bar_text(a, ACCENT_WELL[a], bg), ACCENT_WELL[a] };
        out[UI_FOCUS * OVERLAY_ACCENTS + a] =
            (display_overlay_style_t){ bg, wash(ACCENT_BAR[a]) };
        /* One neutral gauge look for every accent: medium white on a
         * dark tint — the fill reads at a glance, the track recedes. */
        out[UI_TRACK * OVERLAY_ACCENTS + a] =
            (display_overlay_style_t){ 0x7BEF, 0x39E7 };

        out[UI_BAR_EDGE   * OVERLAY_ACCENTS + a] =
            (display_overlay_style_t){ ACCENT_BAR[a], bg };
        out[UI_WELL_EDGE  * OVERLAY_ACCENTS + a] =
            (display_overlay_style_t){ ACCENT_WELL[a], bg };
        out[UI_FOCUS_EDGE * OVERLAY_ACCENTS + a] =
            (display_overlay_style_t){ wash(ACCENT_BAR[a]), bg };
        out[UI_PANEL       * OVERLAY_ACCENTS + a] =
            (display_overlay_style_t){ text, panel };
        out[UI_PANEL_MUTED * OVERLAY_ACCENTS + a] =
            (display_overlay_style_t){ panel_muted(text, panel), panel };
        out[UI_PANEL_EDGE  * OVERLAY_ACCENTS + a] =
            (display_overlay_style_t){ panel, bg };
        /* The FOCUS wash with its text halfway to the wash: dim, not gone. */
        out[UI_FOCUS_MUTED * OVERLAY_ACCENTS + a] = (display_overlay_style_t){
            (color_t)(((bg >> 1) & 0x7BEF) +
                      ((wash(ACCENT_BAR[a]) >> 1) & 0x7BEF)),
            wash(ACCENT_BAR[a]) };
    }
}
