/*
 * ui.c — overlay TUI primitives, double-buffered: the shell draws into
 * the back buffer while the ISR composites the front one; ui_present()
 * publishes with a single pointer store and swaps. Without this the ISR
 * caught clear-then-redraw mid-flight and the terminal flashed through.
 */

#include "ui_frame.h"
#include "ui_pure.h"
#include "ui_theme.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ui";

/* The baked style table the ISR reads (DRAM). ui_colors() rebuilds it
 * in place only when the theme pair changes (screen entry); a scan
 * racing that rare rebuild can catch mixed entries for one frame —
 * colors only, self-corrects. A per-frame rebuild would make that
 * window a steady strobe, so same-pair calls must stay early-returns. */
static DRAM_ATTR display_overlay_style_t s_theme[UI_PAL_COUNT];
static color_t s_theme_fg, s_theme_bg;    /* the pair currently baked */
static bool    s_theme_baked = false;

static display_overlay_cell_t *s_buf[2] = { NULL, NULL };
static display_overlay_cell_t *s_draw   = NULL;   /* the back buffer */
static int  s_cols    = 0;
static int  s_rows    = 0;
static bool s_visible = false;
static bool s_scrim   = false;   /* dim the terminal behind this frame */
static uint8_t s_pen  = OVERLAY_COL_DEFAULT;   /* current accent color */

/* Shell animation clock (~10 fps), fed each tick via ui_frame(); the
 * hot-region model reads it for press and focus animation. */
static uint32_t s_frame = 0;

void ui_frame(uint32_t frame) { s_frame = frame; }

esp_err_t ui_init(void)
{
    display_get_text_size(&s_cols, &s_rows);
    if (s_cols <= 0 || s_rows <= 0) {
        ESP_LOGE(TAG, "display text buffer not registered yet");
        return ESP_ERR_INVALID_STATE;
    }

    for (int i = 0; i < 2; i++) {
        s_buf[i] = heap_caps_calloc((size_t)s_cols * s_rows,
                                    sizeof(display_overlay_cell_t),
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_buf[i]) {
            ESP_LOGE(TAG, "no DRAM for %dx%d overlay", s_cols, s_rows);
            return ESP_ERR_NO_MEM;
        }
    }
    s_draw = s_buf[0];
    ESP_LOGI(TAG, "overlay %dx%d (2x%u B)", s_cols, s_rows,
             (unsigned)((size_t)s_cols * s_rows * sizeof(**s_buf)));
    return ESP_OK;
}

int ui_cols(void) { return s_cols; }
int ui_rows(void) { return s_rows; }

void ui_present(void)
{
    if (!s_draw) return;
    /* Publish the freshly drawn buffer; ISR reads it next scan. */
    display_set_overlay_buffer(s_draw, s_cols, s_rows, s_scrim);
    s_visible = true;
    /* Swap: next frame is drawn into the buffer the ISR just stopped using. */
    s_draw = (s_draw == s_buf[0]) ? s_buf[1] : s_buf[0];
}

void ui_hide(void)
{
    if (!s_visible) return;
    display_set_overlay_buffer(NULL, 0, 0, false);
    s_visible = false;
}

bool ui_visible(void) { return s_visible; }

#ifdef BUILD_SIMULATOR
bool ui_debug_contains(const char *text)
{
    if (!s_visible || !text || !text[0] || !s_draw) return false;
    size_t len = strlen(text);
    if (len > (size_t)s_cols) return false;

    /* ui_present() swaps s_draw after publishing, so the other buffer is
     * the one the display currently owns. Scanning it catches accidental
     * double-presents that publish a stale frame. */
    const display_overlay_cell_t *front =
        s_draw == s_buf[0] ? s_buf[1] : s_buf[0];
    for (int row = 0; row < s_rows; row++) {
        for (int col = 0; col <= s_cols - (int)len; col++) {
            size_t i = 0;
            while (i < len &&
                   front[row * s_cols + col + (int)i].cp == (uint8_t)text[i])
                i++;
            if (i == len) return true;
        }
    }
    return false;
}

const display_overlay_cell_t *ui_debug_frame(bool *scrim)
{
    if (!s_visible || !s_draw) return NULL;
    if (scrim) *scrim = s_scrim;
    return s_draw == s_buf[0] ? s_buf[1] : s_buf[0];
}

const display_overlay_style_t *ui_debug_styles(void) { return s_theme; }
#endif

void ui_no_cursor(void)
{
    /* Park the terminal cursor off-screen so its blinking XOR block does not
     * flicker through a full-screen modal. vterm restores it on the next
     * session write. */
    display_set_cursor(-1, -1, CURSOR_NONE);
}

void ui_colors(color_t fg, color_t bg)
{
    if (s_theme_baked && fg == s_theme_fg && bg == s_theme_bg) return;
    s_theme_fg    = fg;
    s_theme_bg    = bg;
    s_theme_baked = true;
    ui_theme_build(fg, bg, s_theme);
    display_set_overlay_palette(s_theme, UI_PAL_COUNT);
}

void ui_clear(void)
{
    s_pen   = OVERLAY_COL_DEFAULT;   /* each frame starts on the default */
    s_scrim = false;                 /* and re-decides its backdrop */
    ui_hot_begin(s_frame);           /* ...and its interactive regions */
    if (s_draw)
        memset(s_draw, 0, (size_t)s_cols * s_rows * sizeof(*s_draw));
}

void ui_dim(void)
{
    /* Frame state, not cells: the renderer fades the terminal under every
     * transparent cell of this frame. Draw order does not matter. */
    s_pen   = OVERLAY_COL_DEFAULT;
    s_scrim = true;
}

void ui_pen(uint8_t color) { s_pen = color; }

void ui_putch(int col, int row, uint16_t cp, uint8_t style)
{
    /* (style, pen) → one baked palette entry; UI_BOLD is the only flag
     * that reaches the display (docs/overlay-style.md). Rejected on BOTH
     * builds: out-of-vocabulary codes must fail loudly, not clamp to
     * palette entry 0. In-range OR aliases (cyberdeck_ui.h) are beyond
     * any runtime check — the non-composability rule is the guard. */
    assert((style & ~(UI_STYLE_MASK | UI_BOLD)) == 0 &&
           (style & UI_STYLE_MASK) < UI_STYLE_COUNT);
    assert(s_pen < OVERLAY_ACCENTS);
    if (s_draw && col >= 0 && col < s_cols && row >= 0 && row < s_rows) {
        display_overlay_cell_t *c = &s_draw[row * s_cols + col];
        c->cp    = cp;
        c->attrs = (style & UI_BOLD) ? OVERLAY_ATTR_BOLD : 0;
        c->pal   = (uint8_t)((style & UI_STYLE_MASK) * OVERLAY_ACCENTS + s_pen);
    }
}

void ui_puts(int col, int row, const char *s, uint8_t attrs)
{
    while (*s && col < s_cols)
        ui_putch(col++, row, (uint8_t)*s++, attrs);
}

void ui_puts_u8(int col, int row, const char *s, uint8_t attrs)
{
    uint16_t cp;
    while (col < s_cols && (cp = ui_utf8_next(&s)) != 0)
        ui_putch(col++, row, cp, attrs);
}

void ui_printf(int col, int row, uint8_t attrs, const char *fmt, ...)
{
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ui_puts(col, row, buf, attrs);
}

void ui_fill(int col, int row, int w, int h, uint8_t attrs)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            ui_putch(col + c, row + r, ' ', attrs);
}

int ui_chip(int col, int row, uint16_t left_cp, const char *text,
            uint16_t right_cp, uint8_t style)
{
    /* The caps stay accent TEXT; the body is a BAR unless the caller
     * substitutes another bar-family style (UI_FOCUS). */
    uint8_t bar = (style & UI_STYLE_MASK) ? style : (uint8_t)(UI_BAR | style);
    int x = col;
    if (left_cp) ui_putch(x++, row, left_cp, UI_TEXT);
    ui_putch(x++, row, ' ', bar);
    ui_puts(x, row, text, bar);
    x += (int)strlen(text);
    ui_putch(x++, row, ' ', bar);
    if (right_cp) ui_putch(x++, row, right_cp, UI_TEXT);
    return x;
}
