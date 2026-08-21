/*
 * ISR render bench stress mode — see bench_stress.h.
 */
#include "bench_stress.h"

#ifdef CONFIG_CYBERDECK_BENCH_STRESS

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "display.h"
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
 * Overlay phases.
 *
 * build_row_cache() composites the overlay ABOVE the terminal cell buffer,
 * one decode per column either way — but the two layers do NOT cost the same
 * per cell, and only the overlay-off path had ever been measured. Each phase
 * isolates one branch of render_cache.c:229-277:
 *
 *   off     s_overlay.buf == NULL — the published baseline. Two predicted
 *           null checks per column, nothing else.
 *   clear   registered, every cell transparent (what ui_clear() produces).
 *           Adds the per-column ov_row[] load stream, nothing more.
 *   scrim   transparent + DIM — the modal backdrop (ui_dim()). clear, plus
 *           the scrim branch of resolve_terminal_cell().
 *   spaces  fully opaque U+0020. The overlay branch has NO blank fast path,
 *           so every one of these pays a full font_decode_glyph() where the
 *           identical space in the terminal layer is a word zero-fill. This
 *           is what a space-padded ui_printf() actually costs.
 *   dense   fully opaque glyph soup — worst-case chrome, max decode rate.
 *   bars    opaque spaces + INVERSE across all 8 accents — drives the most
 *           expensive resolve_overlay_cell() branch (bar palette + tint).
 *   bold    dense + BOLD — the bold lookup / synthesize-and-smear path.
 *
 * The terminal underneath is kept dense in every phase, so the transparent
 * phases measure real compositing rather than a blank screen.
 */
typedef enum {
    OV_OFF = 0, OV_CLEAR, OV_SCRIM, OV_SPACES, OV_DENSE, OV_BARS, OV_BOLD,
    OV_COUNT
} ov_phase_t;

static display_overlay_cell_t *s_ov;
static int s_ov_cols, s_ov_rows;

/* Rebuild and re-register the overlay for @p phase. Detached first: the ISR
 * must never read a buffer mid-rewrite, and a phase change is not a hot path. */
static void ov_apply(ov_phase_t phase)
{
    display_set_overlay_buffer(NULL, 0, 0);
    if (phase == OV_OFF || !s_ov)
        return;

    const int n = s_ov_cols * s_ov_rows;
    for (int i = 0; i < n; i++) {
        display_overlay_cell_t c = {
            .cp = 0, .attrs = 0, .color = OVERLAY_COL_DEFAULT
        };
        switch (phase) {
        case OV_CLEAR:
            break;
        case OV_SCRIM:
            c.attrs = OVERLAY_ATTR_DIM;
            break;
        case OV_SPACES:
            c.cp = 0x20;
            break;
        case OV_DENSE:
            c.cp = (uint16_t)(uint8_t)s_set[(unsigned)i % SET_LEN];
            break;
        case OV_BARS:
            c.cp    = 0x20;
            c.attrs = OVERLAY_ATTR_INVERSE;
            c.color = (uint8_t)((unsigned)i % OVERLAY_PAL_SIZE);
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
 * Terminal content modes.
 *
 * DENSE is the historic stress screen: every cell a non-blank glyph with a
 * per-cell SGR change and 25% bold — strictly denser than any real terminal
 * screen, so its max is a worst-case chunk time.
 *
 * SPARSE and BLANK exist because blank cells take the zero-fill fast path in
 * build_row_cache() instead of a glyph decode, which makes ISR cost strongly
 * content-dependent. The pre-2026-08-15 figures in docs/speedupsall.md were
 * taken on REAL SESSION content (btop / ls -lR / idle) because this stress
 * screen did not exist yet — comparing them against DENSE is a cross-workload
 * comparison, not a regression. These two modes bracket that range.
 */
typedef enum { TERM_DENSE = 0, TERM_SPARSE, TERM_BLANK } term_mode_t;

static void feed_terminal(int cols, int rows, unsigned frame, term_mode_t tm)
{
    static char line[1024];

    if (tm == TERM_BLANK)
        return;   /* cleared once at phase entry; nothing to repaint */

    for (int r = 0; r < rows; r++) {
        int n = snprintf(line, sizeof line, "\x1b[%d;1H", r + 1);
        for (int c = 0; c < cols && n < (int)sizeof line - 16; c++) {
            /* SPARSE: ~1 glyph in 6, no SGR churn — a shell screen, not a
             * painted one. DENSE: every cell, with a colour change. */
            if (tm == TERM_SPARSE) {
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

/* Phase schedule: the seven overlay phases over a DENSE terminal, then two
 * overlay-off phases that vary the TERMINAL content instead. The last two are
 * what make the historic real-session figures interpretable. */
static const struct {
    ov_phase_t  ov;
    term_mode_t tm;
    const char *name;
} s_phases[] = {
    { OV_OFF,    TERM_DENSE,  "off"      },
    { OV_CLEAR,  TERM_DENSE,  "clear"    },
    { OV_SCRIM,  TERM_DENSE,  "scrim"    },
    { OV_SPACES, TERM_DENSE,  "spaces"   },
    { OV_DENSE,  TERM_DENSE,  "dense"    },
    { OV_BARS,   TERM_DENSE,  "bars"     },
    { OV_BOLD,   TERM_DENSE,  "bold"     },
    { OV_OFF,    TERM_SPARSE, "t:sparse" },
    { OV_OFF,    TERM_BLANK,  "t:blank"  },
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
    if (!s_ov)
        ESP_LOGW(TAG, "no DRAM for %dx%d overlay — measuring 'off' only",
                 cols, rows);
    else
        display_set_overlay_colors(display_ansi_to_rgb565(0),
                                   display_ansi_to_rgb565(6));

    for (unsigned p = 0;; p = (p + 1) % (sizeof s_phases / sizeof *s_phases)) {
        const ov_phase_t  ov = s_phases[p].ov;
        const term_mode_t tm = s_phases[p].tm;

        if (ov != OV_OFF && !s_ov)
            continue;

        ov_apply(ov);
        if (tm == TERM_BLANK)
            vterm_write("\x1b[2J\x1b[H", 7);   /* clear once, then leave it */

        /* Let the new overlay/content reach the ISR before counting. */
        for (int i = 0; i < SETTLE_TICKS; i++) {
            feed_terminal(cols, rows, frame++, tm);
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        display_render_bench_reset();

        for (int i = 0; i < WINDOW_TICKS; i++) {
            feed_terminal(cols, rows, frame++, tm);
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        uint32_t avg, mx, chunks;
        display_render_bench_get(&avg, &mx, &chunks);
        ESP_LOGI(TAG, "font=%s ph=%-8s avg=%u cyc (%u us) max=%u cyc (%u us) chunks=%u",
                 font_size_name(font_active_size()), s_phases[p].name,
                 (unsigned)avg, (unsigned)(avg / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
                 (unsigned)mx,  (unsigned)(mx  / CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ),
                 (unsigned)chunks);
    }
}

bool bench_stress_start(void)
{
    ESP_LOGW(TAG, "BENCH STRESS build: font=%s — no shell, no network",
             font_size_name(font_active_size()));
    xTaskCreatePinnedToCore(bench_task, "bench", 8192, NULL, 5, NULL, 0);
    return true;
}

#endif /* CONFIG_CYBERDECK_BENCH_STRESS */
