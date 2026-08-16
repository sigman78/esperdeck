/*
 * vterm -- VT/ANSI terminal emulator backed by tsm.
 *
 * tsm_feed() parses the byte stream into tsm's own cell grid.
 * On flush, dirty rows are copied from tsm's cell grid into s_buffer
 * (terminal_cell_t[]) which is registered with display_set_text_buffer().
 * The display ISR reads s_buffer on every frame.
 *
 * Two feeding modes:
 *   vterm_write()          — feed + present (legacy, one-shot writes)
 *   vterm_feed()/_flush()  — batch: the SSH drain loop feeds many chunks,
 *                            then presents once per wake (docs/speedup-render.md)
 */

#include "vterm.h"
#include "display.h"
#include "display_fx.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>
#include "tsm.h"

/* Bench instrumentation */

#ifdef CONFIG_VTERM_BENCH
#include "esp_cpu.h"
typedef struct {
    uint32_t flush_count;
    uint32_t bytes_fed;
    uint64_t tsm_cycles;
    uint64_t draw_cycles;
} vterm_bench_t;
static vterm_bench_t s_bench;
#endif

static const char *TAG = "vterm";

/* State */

static int                 s_cols;
static int                 s_rows;
static vterm_response_cb_t s_response_cb;
static void               *s_response_user;
static bool                s_initialized;

static tsm_t              *s_tsm;
static terminal_cell_t    *s_buffer;

static void tsm_response_forward(const char *data, size_t len, void *user)
{
    (void)user;
    if (s_response_cb)
        s_response_cb(data, len, s_response_user);
}

/* Display refresh */

static inline void refresh_display(void)
{
    const tsm_row_dirty_t *dirty = tsm_dirty(s_tsm);
#ifdef CONFIG_VTERM_BENCH
    uint32_t t0 = esp_cpu_get_cycle_count();
#endif
#if DISPLAY_FX_ROW_GLOW
    /* Row-glow stamping: a bulk repaint (scroll, clear, paging) marks most
     * rows dirty in one flush — stamping those would flash the whole screen
     * instead of highlighting fresh output. Stamp only sparse flushes. */
    int ndirty = 0;
    for (int row = 0; row < s_rows; row++)
        if (dirty[row].l <= dirty[row].r) ndirty++;
    const bool stamp = ndirty <= s_rows / 3;
#endif

    for (int row = 0; row < s_rows; row++) {
        int l = (int)dirty[row].l;
        int r = (int)dirty[row].r;
        if (l > r) continue;
#if DISPLAY_FX_ROW_GLOW
        if (stamp)
            display_fx_touch_row(row);   /* drives the row-recency back glow */
#endif
        const tsm_cell_t *src = tsm_row(s_tsm, row) + l;
        terminal_cell_t  *dst = s_buffer          + row * s_cols + l;
        for (int col = l; col <= r; col++, src++, dst++) {
            dst->cp       = src->cp;
            dst->fg_color = src->fg;
            dst->bg_color = src->bg;
            dst->attrs    = src->attrs;
        }
    }
#ifdef CONFIG_VTERM_BENCH
    uint32_t t1 = esp_cpu_get_cycle_count();
    s_bench.draw_cycles += (t1 - t0);
#endif
    vterm_cursor_refresh();
    tsm_clear_dirty(s_tsm);
}

void vterm_cursor_refresh(void)
{
    if (!s_initialized) return;
    int cx, cy; bool vis;
    tsm_cursor(s_tsm, &cx, &cy, &vis);
    display_set_cursor(cx, cy, vis ? CURSOR_BLOCK : CURSOR_NONE);
}

/* Public API */

esp_err_t vterm_init(int cols, int rows)
{
    ESP_LOGI(TAG, "Initializing vterm (%dx%d)", cols, rows);

    s_cols        = cols;
    s_rows        = rows;
    s_initialized = false;

    /* The display ISR reads this buffer — it must live in internal DRAM,
     * never behind the PSRAM cache. */
    s_buffer = heap_caps_malloc((size_t)cols * (size_t)rows * sizeof(terminal_cell_t),
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_buffer) {
        ESP_LOGE(TAG, "Failed to allocate cell buffer");
        return ESP_ERR_NO_MEM;
    }

    color_t def_fg = display_ansi_to_rgb565(7);
    color_t def_bg = display_ansi_to_rgb565(0);
    for (int i = 0; i < cols * rows; i++) {
        s_buffer[i].cp       = 0x0020;
        s_buffer[i].fg_color = def_fg;
        s_buffer[i].bg_color = def_bg;
        s_buffer[i].attrs    = 0;
    }

    s_tsm = tsm_new(cols, rows);
    if (!s_tsm) {
        ESP_LOGE(TAG, "tsm_new failed");
        free(s_buffer);
        return ESP_ERR_NO_MEM;
    }
    tsm_set_response_cb(s_tsm, tsm_response_forward, NULL);

    display_set_text_buffer(s_buffer, cols, rows);
    display_set_cursor(0, 0, CURSOR_BLOCK);
    s_initialized = true;
    ESP_LOGI(TAG, "vterm ready: %dx%d", cols, rows);
    return ESP_OK;
}

void vterm_feed(const char *data, size_t len)
{
    if (!s_initialized) return;

    /* Visual bell: tsm swallows BEL (0x07) silently, so trigger the alert
     * here — the red BEL tag plus a half-length static burst (a no-op when
     * disabled in the fx config). A bare BEL in the stream is the terminal
     * bell; the rare 0x07 inside an escape sequence just gives a harmless
     * extra flash. */
    for (size_t i = 0; i < len; i++)
        if (data[i] == 0x07) { display_bell(); display_fx_static_brief(); break; }

#ifdef CONFIG_VTERM_BENCH
    uint32_t t0 = esp_cpu_get_cycle_count();
#endif
    tsm_feed(s_tsm, (const uint8_t *)data, len);
#ifdef CONFIG_VTERM_BENCH
    uint32_t t1 = esp_cpu_get_cycle_count();
    s_bench.tsm_cycles += (t1 - t0);
    s_bench.bytes_fed  += len;
#endif
}

void vterm_flush(void)
{
    if (!s_initialized) return;
    /* ?2026 synchronized update open — hold the frame until ESU. */
    if (tsm_sync_update(s_tsm)) return;
#ifdef CONFIG_VTERM_BENCH
    s_bench.flush_count++;
#endif
    refresh_display();
}

void vterm_write(const char *data, size_t len)
{
    vterm_feed(data, len);
    vterm_flush();
}

void vterm_set_response_cb(vterm_response_cb_t cb, void *user)
{
    s_response_cb   = cb;
    s_response_user = user;
}

void vterm_reset(void)
{
    if (!s_initialized) return;
    tsm_reset(s_tsm);
    refresh_display();
}

bool vterm_app_cursor_keys(void)
{
    if (!s_initialized) return false;
    return tsm_app_cursor_keys(s_tsm);
}

void vterm_bench_report(void)
{
#ifdef CONFIG_VTERM_BENCH
    /* Silent when nothing was fed: the stat block runs every 30 s and each
     * log line blocks the read task on the UART (~9 ms/100 chars). */
    if (s_bench.bytes_fed == 0) return;

    /* Parse-vs-state split: tsm_cycles minus the vtable shims left over is
     * the pure vtparse share. */
    tsm_bench_t tb;
    tsm_bench_get(&tb);
    uint64_t shim_cycles = tb.print_cycles + tb.csi_cycles + tb.c0_cycles + tb.other_cycles;
    uint64_t parse_cycles = (s_bench.tsm_cycles > shim_cycles)
                           ? (s_bench.tsm_cycles - shim_cycles) : 0;
    ESP_LOGI("vterm_bench",
        "KB=%" PRIu32 " fl=%" PRIu32 " tsm=%" PRIu32 "ms dr=%" PRIu32
        " pr=%" PRIu32 " csi=%" PRIu32 " c0=%" PRIu32 " par=%" PRIu32
        " scr=%" PRIu32 "/%" PRIu32 " rows=%" PRIu32,
        s_bench.bytes_fed / 1024u,
        s_bench.flush_count,
        (uint32_t)(s_bench.tsm_cycles  / 240000),
        (uint32_t)(s_bench.draw_cycles / 240000),
        (uint32_t)(tb.print_cycles     / 240000),
        (uint32_t)(tb.csi_cycles       / 240000),
        (uint32_t)(tb.c0_cycles        / 240000),
        (uint32_t)(parse_cycles        / 240000),
        tb.scroll_up_calls, tb.scroll_down_calls, (uint32_t)tb.scroll_rows);
#endif
}

void vterm_bench_reset(void)
{
#ifdef CONFIG_VTERM_BENCH
    memset(&s_bench, 0, sizeof(s_bench));
    tsm_bench_reset();
#endif
}
