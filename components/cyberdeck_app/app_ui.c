/*
 * app_ui.c — overlay TUI primitives, double-buffered: the shell draws into
 * the back buffer while the ISR composites the front one; ui_present()
 * publishes with a single pointer store and swaps. Without this the ISR
 * caught clear-then-redraw mid-flight and the terminal flashed through.
 */

#include "app_ui.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "app_ui";

static display_overlay_cell_t *s_buf[2] = { NULL, NULL };
static display_overlay_cell_t *s_draw   = NULL;   /* we write here (back)  */
static int  s_cols    = 0;
static int  s_rows    = 0;
static bool s_visible = false;
static uint8_t s_pen  = OVERLAY_COL_DEFAULT;   /* current accent color */

/* Shell animation clock (~10 fps), fed each tick via ui_frame(); drives the
 * selected-tile body marquee. */
static uint32_t s_frame = 0;

void ui_frame(uint32_t frame) { s_frame = frame; }

/* Bounce marquee: park at the head, crawl to the tail, park, crawl back.
 * One owner slot keyed by tile origin; the epoch resets when another tile
 * (or the same tile after a gap) takes it, so a fresh selection starts at
 * the head. */
#define MQ_STEP_FRAMES 3   /* frames per character step (~3 cells/s) */
#define MQ_DWELL_STEPS 5   /* extra steps parked at each end (~1.5 s) */

static int      s_mq_col = -1, s_mq_row = -1;  /* owner tile origin */
static uint32_t s_mq_zero = 0;                 /* owner's epoch frame */
static uint32_t s_mq_seen = 0;                 /* owner last drawn at */

static int marquee_offset(int col, int row, int overflow)
{
    if (col != s_mq_col || row != s_mq_row ||
            s_frame - s_mq_seen > MQ_STEP_FRAMES) {
        s_mq_col  = col;
        s_mq_row  = row;
        s_mq_zero = s_frame;
    }
    s_mq_seen = s_frame;

    uint32_t ph = ((s_frame - s_mq_zero) / MQ_STEP_FRAMES)
                  % (2u * (uint32_t)(overflow + MQ_DWELL_STEPS));
    if (ph < MQ_DWELL_STEPS) return 0;                 /* head dwell */
    ph -= MQ_DWELL_STEPS;
    if (ph < (uint32_t)overflow) return (int)ph + 1;   /* crawl to tail */
    ph -= (uint32_t)overflow;
    if (ph < MQ_DWELL_STEPS) return overflow;          /* tail dwell */
    return overflow - 1 - (int)(ph - MQ_DWELL_STEPS);  /* crawl back */
}

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
    display_set_overlay_buffer(s_draw, s_cols, s_rows);
    s_visible = true;
    /* Swap: next frame is drawn into the buffer the ISR just stopped using. */
    s_draw = (s_draw == s_buf[0]) ? s_buf[1] : s_buf[0];
}

void ui_hide(void)
{
    if (!s_visible) return;
    display_set_overlay_buffer(NULL, 0, 0);
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
    display_set_overlay_colors(fg, bg);
}

void ui_clear(void)
{
    s_pen = OVERLAY_COL_DEFAULT;   /* each frame starts on the screen default */
    if (s_draw)
        memset(s_draw, 0, (size_t)s_cols * s_rows * sizeof(*s_draw));
}

void ui_dim(void)
{
    /* Transparent scrim: cells stay see-through (cp==0) but flagged DIM, so
     * the renderer fades the terminal behind them. Draw chrome on top. */
    s_pen = OVERLAY_COL_DEFAULT;
    if (!s_draw) return;
    for (int i = 0; i < s_cols * s_rows; i++) {
        s_draw[i].cp    = 0;
        s_draw[i].attrs = OVERLAY_ATTR_DIM;
        s_draw[i].color = 0;
    }
}

void ui_pen(uint8_t color) { s_pen = color; }

void ui_putch(int col, int row, uint16_t cp, uint8_t attrs)
{
    if (s_draw && col >= 0 && col < s_cols && row >= 0 && row < s_rows) {
        s_draw[row * s_cols + col].cp    = cp;
        s_draw[row * s_cols + col].attrs = attrs;
        s_draw[row * s_cols + col].color = s_pen;
    }
}

void ui_puts(int col, int row, const char *s, uint8_t attrs)
{
    while (*s && col < s_cols)
        ui_putch(col++, row, (uint8_t)*s++, attrs);
}

void ui_puts_u8(int col, int row, const char *s, uint8_t attrs)
{
    const uint8_t *p = (const uint8_t *)s;
    while (*p && col < s_cols) {
        uint16_t cp;
        if (p[0] < 0x80) {
            cp = p[0];
            p += 1;
        } else if ((p[0] & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            cp = (uint16_t)(((p[0] & 0x1F) << 6) | (p[1] & 0x3F));
            p += 2;
        } else if ((p[0] & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 &&
                   (p[2] & 0xC0) == 0x80) {
            cp = (uint16_t)(((p[0] & 0x0F) << 12) |
                            ((p[1] & 0x3F) << 6) | (p[2] & 0x3F));
            p += 3;
        } else {
            /* Beyond-BMP (4-byte) or malformed: one '?', resync past any
             * continuation bytes. */
            cp = '?';
            p += 1;
            while ((*p & 0xC0) == 0x80) p++;
        }
        ui_putch(col++, row, cp, attrs);
    }
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

void ui_hline(int col, int row, int width,
              uint16_t left_cp, uint16_t fill_cp, uint16_t right_cp)
{
    ui_putch(col, row, left_cp, 0);
    for (int i = 1; i < width - 1; i++)
        ui_putch(col + i, row, fill_cp, 0);
    ui_putch(col + width - 1, row, right_cp, 0);
}

void ui_fill(int col, int row, int w, int h, uint8_t attrs)
{
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            ui_putch(col + c, row + r, ' ', attrs);
}

void ui_box(int col, int row, int w, int h, const char *title)
{
    ui_hline(col, row, w, UI_BOX_TL, UI_BOX_H, UI_BOX_TR);
    for (int r = 1; r < h - 1; r++) {
        ui_putch(col, row + r, UI_BOX_V, 0);
        for (int c = 1; c < w - 1; c++)
            ui_putch(col + c, row + r, ' ', 0);
        ui_putch(col + w - 1, row + r, UI_BOX_V, 0);
    }
    ui_hline(col, row + h - 1, w, UI_BOX_BL, UI_BOX_H, UI_BOX_BR);

    if (title && *title) {
        int tlen = (int)strlen(title);
        int toff = (w - 2 - tlen) / 2;
        if (toff < 0) toff = 0;
        ui_puts(col + 1 + toff, row, title, 0);
    }
}

int ui_chip(int col, int row, uint16_t left_cp, const char *text,
            uint16_t right_cp, uint8_t attrs)
{
    int x = col;
    if (left_cp) ui_putch(x++, row, left_cp, 0);
    ui_putch(x++, row, ' ', OVERLAY_ATTR_INVERSE);
    ui_puts(x, row, text, OVERLAY_ATTR_INVERSE | attrs);
    x += (int)strlen(text);
    ui_putch(x++, row, ' ', OVERLAY_ATTR_INVERSE);
    if (right_cp) ui_putch(x++, row, right_cp, 0);
    return x;
}

void ui_tile(int col, int row, int w, int h,
             const char *title, const char *body, bool selected)
{
    /* DOS-style solid button: a colored bar (INVERSE puts the pen accent in
     * the background). Focus = pastel wash (BRIGHT) + a lit left rail. */
    uint8_t a = OVERLAY_ATTR_INVERSE | (selected ? OVERLAY_ATTR_BRIGHT : 0);

    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            ui_putch(col + c, row + r, ' ', a);

    if (selected) {
        /* ▐ in INVERSE+WHITE: set pixels (right half) take the dark fg,
         * clear pixels (left half) the white bg → white rail + seam. */
        uint8_t old_pen = s_pen;
        s_pen = OVERLAY_COL_WHITE;
        for (int r = 0; r < h; r++)
            ui_putch(col, row + r, UI_RHALF, OVERLAY_ATTR_INVERSE);
        s_pen = old_pen;
    }

    /* Title (+ optional body) vertically centered; bold title, regular
     * body — heading + detail. */
    int tx    = col + 2;
    int inner = w - 3;
    if (inner < 1) return;
    char t[80];
    int lines = (body && *body && h >= 2) ? 2 : 1;
    int ty    = row + (h - lines) / 2;
    if (title && *title) {
        snprintf(t, sizeof(t), "%-.*s", inner, title);
        ui_puts(tx, ty, t, a | OVERLAY_ATTR_BOLD);
    }
    if (lines == 2) {
        /* An overlong body bounce-scrolls while selected; unselected tiles
         * show the truncated head. */
        int off = 0, blen = (int)strlen(body);
        if (selected && blen > inner)
            off = marquee_offset(col, row, blen - inner);
        snprintf(t, sizeof(t), "%-.*s", inner, body + off);
        ui_puts(tx, ty + 1, t, a);
    }
}

void ui_field(int col, int row, int width, const char *text,
              int cursor, bool focused, bool mask)
{
    if (width < 3) return;
    int inner = width - 2;                 /* space between the [ ] brackets */
    int len   = (int)strlen(text);
    if (cursor < 0)   cursor = 0;
    if (cursor > len) cursor = len;

    /* Scroll so the caret stays visible — but ONLY for the focused field.
     * An unfocused field always shows from its start, else every repaint
     * scrolls it by the (focused) field's caret and hides its head. */
    int start = 0;
    if (focused && cursor > inner - 1) start = cursor - (inner - 1);

    uint8_t in = focused ? OVERLAY_ATTR_INVERSE : 0;
    ui_putch(col, row, '[', 0);
    ui_putch(col + width - 1, row, ']', 0);
    for (int i = 0; i < inner; i++) {
        int idx = start + i;
        uint16_t cp = ' ';
        if (idx < len) cp = mask ? '*' : (uint8_t)text[idx];
        /* Block cursor on the focused field's caret cell. */
        uint8_t a = (focused && idx == cursor) ? (in ^ OVERLAY_ATTR_INVERSE) : in;
        ui_putch(col + 1 + i, row, cp, a);
    }
}
