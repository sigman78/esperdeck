/*
 * ISR render bench stress mode — see bench_stress.h.
 */
#include "bench_stress.h"

#ifdef CONFIG_CYBERDECK_BENCH_STRESS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "display.h"
#include "display_fx.h"
#include "vterm.h"

static const char *TAG = "bench_stress";

font_size_t bench_stress_font_override(font_size_t normal)
{
    (void)normal;
#if defined(CONFIG_CYBERDECK_BENCH_STRESS_FONT_12X24)
    return FONT_SIZE_12X24;
#elif defined(CONFIG_CYBERDECK_BENCH_STRESS_FONT_10X20)
    return FONT_SIZE_10X20;
#else
    return FONT_SIZE_8X16;
#endif
}

/* Glyph soup shared by the terminal feed and the dense overlay phases. */
static const char s_set[] = "@WM#g&%8QRBdKXhE";
#define SET_LEN  ((unsigned)(sizeof s_set - 1))

/*
 * Overlay phases — each isolates one branch of build_row_cache()'s overlay
 * resolve, over a terminal kept dense so the transparent phases measure real
 * compositing. The per-phase table is docs/bench-methodology.md.
 */
typedef enum {
    OV_OFF = 0, OV_CLEAR, OV_SCRIM, OV_SPACES, OV_DENSE, OV_BARS, OV_BOLD,
    OV_COUNT
} ov_phase_t;

static display_overlay_cell_t *s_ov;
static int s_ov_cols, s_ov_rows;

/* Bench-local style table: black text on the 8 ANSI backgrounds. */
#define BENCH_OV_PAL 8
static display_overlay_style_t s_ov_pal[BENCH_OV_PAL];

/* Rebuild and re-register the overlay for @p phase. Detached first: the ISR
 * must never read a buffer mid-rewrite, and a phase change is not a hot path. */
static void ov_apply(ov_phase_t phase)
{
    display_set_overlay_buffer(NULL, 0, 0);
    if (phase == OV_OFF || !s_ov)
        return;

    const int n = s_ov_cols * s_ov_rows;
    for (int i = 0; i < n; i++) {
        /* Entry 6 = black-on-cyan, matching the pre-palette bench look. */
        display_overlay_cell_t c = { .cp = 0, .attrs = 0, .pal = 6 };
        switch (phase) {
        case OV_CLEAR:
            break;
        case OV_SCRIM:
            c.attrs = OVERLAY_ATTR_SCRIM;
            break;
        case OV_SPACES:
            c.cp = 0x20;
            break;
        case OV_DENSE:
            c.cp = (uint16_t)(uint8_t)s_set[(unsigned)i % SET_LEN];
            break;
        case OV_BARS:
            c.cp  = 0x20;
            c.pal = (uint8_t)((unsigned)i % BENCH_OV_PAL);
            break;
        case OV_BOLD:
            c.cp    = (uint16_t)(uint8_t)s_set[(unsigned)i % SET_LEN];
            c.attrs = OVERLAY_ATTR_BOLD;
            break;
        default:
            break;
        }
        s_ov[i] = c;
    }
    display_set_overlay_buffer(s_ov, s_ov_cols, s_ov_rows);
}

/*
 * Terminal content modes. DENSE is the worst-case stress screen; SPARSE and
 * BLANK bracket the ~29% content dependence (blank cells skip the decode);
 * MIXED sizes the glyph cache. Definitions and the cross-workload caveat for
 * pre-2026-08-15 figures: docs/bench-methodology.md.
 */
typedef enum { TERM_DENSE = 0, TERM_SPARSE, TERM_BLANK, TERM_MIXED } term_mode_t;

/* A TUI-shaped repertoire: 160 distinct codepoints spanning ASCII, box
 * drawing, block elements and Latin-1 — what mc / btop / tmux actually put on
 * screen. Deliberately wider than the 95-entry ASCII glyph cache, so it shows
 * what non-ASCII content costs.
 *
 * Codepoint ranges, in order: ASCII 94, box drawing 128, blocks 32,
 * Latin-1 64, Greek 64, Cyrillic 128. */
static uint16_t mixed_cp(unsigned k, unsigned span)
{
    k %= span;
    if (k <  94) return (uint16_t)(0x0021 + k);
    k -= 94;
    if (k < 128) return (uint16_t)(0x2500 + k);
    k -= 128;
    if (k <  32) return (uint16_t)(0x2580 + k);
    k -= 32;
    if (k <  64) return (uint16_t)(0x00C0 + k);
    k -= 64;
    if (k <  64) return (uint16_t)(0x0390 + k);
    k -= 64;
    return (uint16_t)(0x0410 + (k % 128u));
}

static int utf8_put(char *p, uint16_t cp)
{
    if (cp < 0x80)  { p[0] = (char)cp; return 1; }
    if (cp < 0x800) { p[0] = (char)(0xC0 | (cp >> 6));
                      p[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    p[0] = (char)(0xE0 | (cp >> 12));
    p[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    p[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
}

static void feed_terminal(int cols, int rows, unsigned frame, term_mode_t tm,
                          unsigned span)
{
    static char line[1024];

    if (tm == TERM_BLANK)
        return;   /* cleared once at phase entry; nothing to repaint */

    for (int r = 0; r < rows; r++) {
        int n = snprintf(line, sizeof line, "\x1b[%d;1H", r + 1);
        for (int c = 0; c < cols && n < (int)sizeof line - 16; c++) {
            /* SPARSE: ~1 glyph in 6, no SGR churn — a shell screen, not a
             * painted one. DENSE: every cell, with a colour change. */
            if (tm == TERM_MIXED) {
                n += utf8_put(line + n, mixed_cp(r * 13u + c + frame, span));
            } else if (tm == TERM_SPARSE) {
                const bool ink = ((r * 7u + c * 3u + frame) % 6u) == 0;
                n += snprintf(line + n, sizeof line - n, "%c",
                              ink ? s_set[(r + c) % SET_LEN] : ' ');
            } else {
                n += snprintf(line + n, sizeof line - n, "\x1b[%d;%dm%c",
                              ((c + r) & 3) == 0,
                              31 + (int)((r + c + frame) % 7u),
                              s_set[(r * 3 + c + frame) % SET_LEN]);
            }
        }
        vterm_feed(line, (size_t)n);
    }
    vterm_flush();   /* vterm_feed only parses; flush pushes the cells */
}

/*
 * Effect config under test. A bench build never calls display_fx_set(), so
 * each phase names its own complete config explicitly — no phase inherits
 * another's. (This is also why every figure recorded before 2026-08-21 had
 * wobble off: docs/bench-methodology.md.)
 */
typedef enum { FX_NOWOB = 0, FX_APP, FX_NOSCAN, FX_NONE } fx_mode_t;

static void fx_apply(fx_mode_t m)
{
    display_fx_cfg_t c;
    display_fx_defaults(&c);          /* scanlines 1, bold_pop 1, wobble 2 */
    switch (m) {
    case FX_APP:                      break;   /* exactly what the shell loads */
    case FX_NOWOB:  c.wobble = 0;              /* == every pre-2026-08-21 run */
                    break;
    case FX_NOSCAN: c.wobble = 0; c.scanlines = 0;
                    break;
    case FX_NONE:   c.wobble = 0; c.scanlines = 0; c.bold_pop = 0;
                    c.glow = 0; c.mono = 0;
                    break;
    }
    display_fx_set(&c);
}

/* The phase schedule runs seven overlay phases over a dense terminal. Two
 * of them vary the TERMINAL content, and four vary the EFFECT config.
 * Everything except the fx: phases runs FX_NOWOB, which reproduces the
 * historic runs. */
static const struct {
    ov_phase_t  ov;
    term_mode_t tm;
    fx_mode_t   fx;
    unsigned    span;      /* distinct codepoints, TERM_MIXED only */
    const char *name;
} s_phases[] = {
    { OV_OFF, TERM_DENSE, FX_NOWOB, 0,   "off" },
    { OV_CLEAR, TERM_DENSE, FX_NOWOB, 0,   "clear" },
    { OV_SCRIM, TERM_DENSE, FX_NOWOB, 0,   "scrim" },
    { OV_SPACES, TERM_DENSE, FX_NOWOB, 0,   "spaces" },
    { OV_DENSE, TERM_DENSE, FX_NOWOB, 0,   "dense" },
    { OV_BARS, TERM_DENSE, FX_NOWOB, 0,   "bars" },
    { OV_BOLD, TERM_DENSE, FX_NOWOB, 0,   "bold" },
    { OV_OFF, TERM_SPARSE, FX_NOWOB, 0,   "t:sparse" },
    { OV_OFF,    TERM_MIXED,  FX_NOWOB, 160, "t:mix160"  },
    { OV_OFF,    TERM_MIXED,  FX_NOWOB, 320, "t:mix320"  },
    { OV_OFF,    TERM_MIXED,  FX_NOWOB, 510, "t:mix510"  },
    { OV_OFF, TERM_BLANK, FX_NOWOB, 0,   "t:blank" },
    { OV_OFF, TERM_DENSE, FX_APP, 0,   "fx:app" },
    { OV_OFF, TERM_SPARSE, FX_APP, 0,   "fx:app+sp" },
    { OV_OFF, TERM_DENSE, FX_NOSCAN, 0,   "fx:noscan" },
    { OV_OFF, TERM_DENSE, FX_NONE, 0,   "fx:none" },
};

#define SETTLE_TICKS   3    /* frames discarded after an overlay switch */
#define WINDOW_TICKS  47    /* measured window: 47 x 100 ms ~= 4.7 s    */

static void bench_task(void *arg)
{
    (void)arg;
    const int cols = display_text_cols(), rows = display_text_rows();
    unsigned frame = 0;

    s_ov_cols = cols;
    s_ov_rows = rows;
    s_ov = heap_caps_calloc((size_t)cols * rows, sizeof(*s_ov),
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_ov) {
        ESP_LOGW(TAG, "no DRAM for %dx%d overlay — measuring 'off' only",
                 cols, rows);
    } else {
        for (int i = 0; i < BENCH_OV_PAL; i++)
            s_ov_pal[i] = (display_overlay_style_t){
                display_ansi_to_rgb565(0), display_ansi_to_rgb565(i) };
        display_set_overlay_palette(s_ov_pal, BENCH_OV_PAL);
    }

    for (unsigned p = 0;; p = (p + 1) % (sizeof s_phases / sizeof *s_phases)) {
        const ov_phase_t  ov = s_phases[p].ov;
        const term_mode_t tm = s_phases[p].tm;

        if (ov != OV_OFF && !s_ov)
            continue;

        fx_apply(s_phases[p].fx);
        ov_apply(ov);
        if (tm == TERM_BLANK)
            vterm_write("\x1b[2J\x1b[H", 7);   /* clear once, then leave it */

        /* Let the new overlay/content reach the ISR before counting. */
        for (int i = 0; i < SETTLE_TICKS; i++) {
            feed_terminal(cols, rows, frame++, tm, s_phases[p].span);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        display_render_bench_reset();

        for (int i = 0; i < WINDOW_TICKS; i++) {
            feed_terminal(cols, rows, frame++, tm, s_phases[p].span);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        uint32_t avg, mx, chunks;
        display_render_bench_get(&avg, &mx, &chunks);
        ESP_LOGI(TAG, "font=%s ph=%-9s avg=%u cyc (%u us) max=%u cyc (%u us) chunks=%u",
                 font_size_name(font_active_size()), s_phases[p].name,
                 (unsigned)avg, (unsigned)(avg / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
                 (unsigned)mx,  (unsigned)(mx  / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
                 (unsigned)chunks);
    }
}

/*
 * Memory-access microbenchmark — run once at startup. Raw uncached-SRAM
 * load/store cost as the render ISR pays it; rationale and reporting format
 * in docs/bench-methodology.md, distilled rules in docs/performance.md.
 */
#ifdef CONFIG_DISPLAY_ISR_BENCH
#include "esp_cpu.h"

#define MB_BYTES  1600                 /* one 800 px RGB565 scanline */
#define MB_RUNS   8

#define MB_TIME(label, stmt)                                                  \
    do {                                                                      \
        uint32_t best = 0xFFFFFFFFu;                                          \
        for (int r = 0; r < MB_RUNS; r++) {                                   \
            const uint32_t t0 = esp_cpu_get_cycle_count();                    \
            stmt;                                                             \
            const uint32_t dt = esp_cpu_get_cycle_count() - t0;               \
            if (dt < best) best = dt;                                         \
        }                                                                     \
        ESP_LOGI(TAG, "  %-22s %6u cyc  %5u.%02u cyc/byte", label,            \
                 (unsigned)best, (unsigned)(best / MB_BYTES),                 \
                 (unsigned)((best * 100u / MB_BYTES) % 100u));                \
    } while (0)

static void membench(void)
{
    uint8_t *a = heap_caps_aligned_alloc(32, MB_BYTES + 64,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *b = heap_caps_aligned_alloc(32, MB_BYTES + 64,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!a || !b) {
        ESP_LOGW(TAG, "membench: no aligned DRAM");
        free(a); free(b);
        return;
    }
    const int n8 = MB_BYTES, n16 = MB_BYTES / 2, n32 = MB_BYTES / 4;

    ESP_LOGI(TAG, "membench: internal SRAM, %d B, min of %d", MB_BYTES, MB_RUNS);

    volatile uint8_t  *d8  = (volatile uint8_t *)a;
    volatile uint16_t *d16 = (volatile uint16_t *)a;
    volatile uint32_t *d32 = (volatile uint32_t *)a;
    MB_TIME("fill  u8",        for (int i = 0; i < n8;  i++) d8[i]  = 0);
    MB_TIME("fill  u16",       for (int i = 0; i < n16; i++) d16[i] = 0);
    MB_TIME("fill  u32 @16B",  for (int i = 0; i < n32; i++) d32[i] = 0);

    /* 4-byte aligned but straddling the 16-byte bus width */
    volatile uint32_t *d32o = (volatile uint32_t *)(a + 4);
    MB_TIME("fill  u32 @4B",   for (int i = 0; i < n32 - 1; i++) d32o[i] = 0);

    MB_TIME("fill  u32 x4",    for (int i = 0; i < n32; i += 4) {
                                   d32[i] = 0; d32[i+1] = 0;
                                   d32[i+2] = 0; d32[i+3] = 0; });

    const volatile uint8_t  *s8  = (const volatile uint8_t *)b;
    const volatile uint16_t *s16 = (const volatile uint16_t *)b;
    const volatile uint32_t *s32 = (const volatile uint32_t *)b;
    MB_TIME("copy  u8",        for (int i = 0; i < n8;  i++) d8[i]  = s8[i]);
    MB_TIME("copy  u16",       for (int i = 0; i < n16; i++) d16[i] = s16[i]);
    MB_TIME("copy  u32 @16B",  for (int i = 0; i < n32; i++) d32[i] = s32[i]);
    MB_TIME("copy  u32 x4",    for (int i = 0; i < n32; i += 4) {
                                   d32[i] = s32[i];     d32[i+1] = s32[i+1];
                                   d32[i+2] = s32[i+2]; d32[i+3] = s32[i+3]; });

    /* Genuinely misaligned 32-bit loads: src +2 bytes. XCHAL says the hardware
     * handles it; this is what it charges. */
    const volatile uint32_t *s32u = (const volatile uint32_t *)(b + 2);
    MB_TIME("copy  u32 src+2",  for (int i = 0; i < n32 - 1; i++) d32[i] = s32u[i]);

    /* Funnel-shift equivalent — what the odd-displacement wobble path did */
    MB_TIME("copy  u32 funnel", for (int i = 0; i < n32 - 1; i++)
                                   d32[i] = (s32[i] >> 16) | (s32[i+1] << 16));

    MB_TIME("ROM memcpy",       memcpy(a, b, MB_BYTES));
    MB_TIME("ROM memset",       memset(a, 0, MB_BYTES));

    free(a);
    free(b);
}
#else
static void membench(void) { }
#endif

bool bench_stress_start(void)
{
    ESP_LOGW(TAG, "BENCH STRESS build: font=%s — no shell, no network",
             font_size_name(font_active_size()));
    membench();
    xTaskCreatePinnedToCore(bench_task, "bench", 8192, NULL, 5, NULL, 0);
    return true;
}

#endif /* CONFIG_CYBERDECK_BENCH_STRESS */
