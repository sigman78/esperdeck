/*
 * app_ui.c — overlay TUI primitives, double-buffered.
 *
 * Two internal-DRAM buffers: the shell draws into the *back* buffer while the
 * display ISR composites the *front* one, so a frame is never shown while it
 * is being rebuilt. ui_present() publishes the back buffer with a single
 * pointer store (the ISR picks it up on its next scan) and swaps. Without
 * this, the ISR caught render_*()'s clear-then-redraw mid-flight and the
 * underlying terminal flashed through every repaint.
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
    /* Transparent scrim: every cell stays see-through (cp==0) but flagged DIM,
     * so the renderer fades the terminal behind it (50% halving by default, or a
     * dithered checkerboard if OVERLAY_DIM_DITHER is set in display_render.c).
     * Draw opaque chrome on top afterwards. */
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
            uint16_t right_cp)
{
    int x = col;
    if (left_cp) ui_putch(x++, row, left_cp, 0);
    ui_putch(x++, row, ' ', OVERLAY_ATTR_INVERSE);
    ui_puts(x, row, text, OVERLAY_ATTR_INVERSE);
    x += (int)strlen(text);
    ui_putch(x++, row, ' ', OVERLAY_ATTR_INVERSE);
    if (right_cp) ui_putch(x++, row, right_cp, 0);
    return x;
}

void ui_tile(int col, int row, int w, int h,
             const char *title, const char *body, bool selected)
{
    uint8_t a = selected ? OVERLAY_ATTR_INVERSE : 0;

    /* Solid fill so the whole tile is a tappable, visible target. */
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            ui_putch(col + c, row + r, ' ', a);

    /* Double-line border — a chunky "mainframe panel" look. */
    ui_putch(col,         row,         UI_DTL, a);
    ui_putch(col + w - 1, row,         UI_DTR, a);
    ui_putch(col,         row + h - 1, UI_DBL, a);
    ui_putch(col + w - 1, row + h - 1, UI_DBR, a);
    for (int c = 1; c < w - 1; c++) {
        ui_putch(col + c, row,         UI_DH, a);
        ui_putch(col + c, row + h - 1, UI_DH, a);
    }
    for (int r = 1; r < h - 1; r++) {
        ui_putch(col,         row + r, UI_DV, a);
        ui_putch(col + w - 1, row + r, UI_DV, a);
    }

    /* First inner column is a marker slot: ▶ when selected. Title/body follow.
     * Everything is truncated to the remaining inner width. */
    int mx    = col + 1;
    int tx    = col + 2;
    int inner = w - 3;
    if (inner < 1) return;
    char t[80];
    if (h >= 2) {
        ui_putch(mx, row + 1, selected ? UI_PLAY : ' ', a);
        if (title && *title) {
            snprintf(t, sizeof(t), "%-.*s", inner, title);
            ui_puts(tx, row + 1, t, a);
        }
    }
    if (body && *body && h >= 3) {
        snprintf(t, sizeof(t), "%-.*s", inner, body);
        ui_puts(tx, row + 2, t, a);
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
