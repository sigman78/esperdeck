/*
 * app_theme.c — bakes the overlay style table: every look the rendition
 * uses, resolved once per ui_colors() call instead of per cell in the
 * ISR. Tones and derivations carried over verbatim from the retired
 * per-cell resolver (docs/overlay-style.md).
 */

#include "app_theme.h"

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

/* Focus wash: 50% toward white (carry-safe half-sum). */
static color_t wash(color_t c)
{
    return (color_t)(((c >> 1) & 0x7BEF) + 0x7BEF);
}

void ui_theme_build(color_t fg, color_t bg, display_overlay_style_t *out)
{
    for (int a = 0; a < OVERLAY_ACCENTS; a++) {
        const color_t text = a ? ACCENT_TEXT[a] : fg;

        out[UI_TEXT  * OVERLAY_ACCENTS + a] =
            (display_overlay_style_t){ text, bg };
        /* Muted = halfway between the accent and the theme bg (carry-safe
         * average). On the black theme this is plain half brightness;
         * on a colored theme it recedes into THAT color instead of into
         * black, which would converge with a dark bg (theme test). */
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
    }
}
