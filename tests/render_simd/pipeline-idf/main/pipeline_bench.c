/* Reuse the shipped stress workload definitions without changing firmware. */
#include "../../../../main/bench_stress.c"
#include "render_internal.h"
#include "display_render.h"
#include "display_fx_internal.h"
#include "scan8.h"
#include "scan_variants.h"
#include "scan8_fused.h"
#include "esp_cpu.h"

static _Alignas(16) uint16_t masks[256][8];
static _Alignas(16) uint16_t golden[12800];
static _Alignas(16) uint16_t output[12800];
static volatile int backend; /* 0 scalar, 1 two-pass SIMD, 2 copy, 3 fused */
static bool verify_scan;
static volatile uint32_t fast_bands, fallback_bands;
static volatile uint32_t fused_bands;
static volatile uint64_t measured_cycles;
static volatile uint32_t measured_count, measured_max;
static portMUX_TYPE stats_lock = portMUX_INITIALIZER_UNLOCKED;
static bool align_lcd_buffers;
static volatile uint32_t seen_alignments;
int production_scan_test(void);

void *__real_heap_caps_aligned_calloc(size_t, size_t, size_t, uint32_t);
void *__wrap_heap_caps_aligned_calloc(size_t alignment, size_t n, size_t size,
                                     uint32_t caps)
{
    if (align_lcd_buffers && n * size == (size_t)display_bounce_px() * 2 && alignment < 16)
        alignment = 16;
    return __real_heap_caps_aligned_calloc(alignment, n, size, caps);
}

void scan8_guarded(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                   const uint32_t (*)[4], const uint16_t (*)[8], unsigned);
int scan8_guard_probe(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                      const uint32_t (*)[4], const uint16_t (*)[8], unsigned);
void __real_render_scan_band(const scan_ctx_t *cx);
void __real_display_render_chunk(color_t *, int, int);

void IRAM_ATTR __wrap_render_scan_band(const scan_ctx_t *cx)
{
    if (BENCH_PRODUCTION) {
        if (verify_scan) {
            render_scan_band_scalar(cx);
            memcpy(golden, cx->dst, (size_t)cx->num_scans * DISPLAY_WIDTH * 2);
        }
        if (!backend) render_scan_band_scalar(cx);
        else __real_render_scan_band(cx);
        if (verify_scan && memcmp(golden, cx->dst, (size_t)cx->num_scans * DISPLAY_WIDTH * 2)) {
            puts("FAIL: integrated SIMD differs from production scalar");
            abort();
        }
        return;
    }
    bool supported = cx->ncols == 800 / g_rs.fw &&
        cx->num_scans == g_rs.band && !(cx->glyph_row0 % g_rs.band);
    if (cx->xoff)
        for (int n = 0; n < cx->num_scans; n++)
            supported &= cx->xoff[n] >= -5 && cx->xoff[n] <= 4;
    if (!backend || !supported || ((uintptr_t)cx->dst & 15)) {
        fallback_bands++;
        __real_render_scan_band(cx);
        return;
    }
    if (verify_scan) {
        __real_render_scan_band(cx);
        memcpy(golden, cx->dst, (size_t)cx->num_scans * 1600);
    }
    bool fused = false;
    if (backend == 3 && g_rs.fw == 8 && cx->xoff)
        for (int n = 0; n < cx->num_scans; n++) fused |= cx->xoff[n] != 0;
    if (fused) {
        for (int n = 0; n < cx->num_scans; n++)
            scan8_fused_line(cx->dst + n * 800, cx->rows + n,
                             cx->pr[cx->scan_on ? n & 1 : 0], masks, cx->xoff[n]);
        fused_bands++;
    } else if (g_rs.fw == 8)
        scan8_guarded(cx->dst, cx->rows, cx->pr[0], cx->pr[cx->scan_on ? 1 : 0],
                      masks, cx->ncols);
    else if (g_rs.fw == 10)
        scan10_guarded(cx->dst, cx->rows, cx->pr[0], cx->pr[cx->scan_on ? 1 : 0],
                       masks, cx->glyph_row0);
    else {
        scan12_guarded(cx->dst, cx->rows, cx->pr[0], cx->pr[cx->scan_on ? 1 : 0],
                       masks, cx->glyph_row0);
        for (int n = 0; n < cx->num_scans; n++)
            memset(cx->dst + n * 800 + 792, 0, 16);
    }
    if (cx->xoff && !fused)
        for (int n = 0; n < cx->num_scans; n++)
            shift_line_guarded(cx->dst + n * 800, cx->xoff[n]);
    fast_bands++;
    if (verify_scan && memcmp(golden, cx->dst, (size_t)cx->num_scans * 1600)) {
        puts("FAIL: production scan differs from guarded SIMD");
        abort();
    }
}

void IRAM_ATTR __wrap_display_render_chunk(color_t *dst, int pos, int bytes)
{
    seen_alignments |= 1u << ((uintptr_t)dst & 15);
    uint32_t start = esp_cpu_get_cycle_count();
    if (backend == 2)
        memcpy(dst, output, bytes);
    else
        __real_display_render_chunk(dst, pos, bytes);
    uint32_t cycles = esp_cpu_get_cycle_count() - start;
    measured_cycles += cycles;
    measured_count++;
    if (cycles > measured_max) measured_max = cycles;
}

static void phase_setup(unsigned p)
{
    fx_apply(s_phases[p].fx);
    ov_apply(s_phases[p].ov);
    vterm_write("\x1b[0m\x1b[2J\x1b[H", 11);
    feed_terminal(display_text_cols(), display_text_rows(), 0, s_phases[p].tm, s_phases[p].span);
}

static void run(void *arg)
{
    (void)arg;
    if (BENCH_PRODUCTION) assert(production_scan_test() == 0);
    else assert(scan_variants_test() == 0);
    if (BENCH_FUSED) {
        assert(BENCH_FONT == 0);
        assert(scan8_fused_test() == 0);
        scan8_fused_bench(output);
    }
    font_init((font_size_t)BENCH_FONT);
    display_render_set_font(font_width(), font_height());
    const int cols = display_text_cols(), rows = display_text_rows();
    const int band_px = display_bounce_px(), band_bytes = band_px * 2;
    ESP_ERROR_CHECK(vterm_init(cols, rows));
    scan8_masks_init(masks);
    s_ov_cols = cols;
    s_ov_rows = rows;
    s_ov = heap_caps_calloc(cols * rows, sizeof(*s_ov), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(s_ov);
    for (int i = 0; i < BENCH_OV_PAL; i++)
        s_ov_pal[i] = (display_overlay_style_t){
            display_ansi_to_rgb565(0), display_ansi_to_rgb565(i) };
    display_set_overlay_palette(s_ov_pal, BENCH_OV_PAL);

    /* Before LCD startup, compare the real scan against SIMD on actual
     * decoded glyphs, both scanline palettes and all stress phases. */
    backend = BENCH_FUSED ? 3 : 1;
    verify_scan = true;
    for (unsigned p = 0; p < sizeof s_phases / sizeof *s_phases; p++) {
        phase_setup(p);
        for (unsigned f = 0; f < 8; f++) {
            for (int pos = 0; pos < 800 * 480; pos += band_px)
                __real_display_render_chunk(output, pos, band_bytes);
            vTaskDelay(1);
        }
    }
    verify_scan = false;
    assert(scan8_guard_probe(output, (const uint8_t *)masks,
                            (const uint32_t (*)[4])masks,
                            (const uint32_t (*)[4])masks, masks, 100) == 0);
    printf("PIPELINE font=%s correctness PASS fast=%lu fallback=%lu fused=%lu\n",
           font_size_name(font_active_size()), (unsigned long)fast_bands, (unsigned long)fallback_bands,
           (unsigned long)fused_bands);
    backend = 0;
    align_lcd_buffers = BENCH_ALIGN_LCD;
    ESP_ERROR_CHECK(display_init());
    align_lcd_buffers = false;
    printf("PIPELINE bounce alignment=%s\n", BENCH_ALIGN_LCD ? "16" : "native");

    /* ABBA order per phase. Same feed sequence and settle period per run.
     * Task and LCD ISR are on core 1; critical sections make snapshots atomic. */
    const int order[] = {BENCH_FUSED ? 1 : 0, BENCH_FUSED ? 3 : 1,
                         BENCH_FUSED ? 3 : 1, BENCH_FUSED ? 1 : 0};
    for (unsigned p = 0; p < sizeof s_phases / sizeof *s_phases; p++) {
        for (unsigned k = 0; k < 4; k++) {
            backend = order[k];
            phase_setup(p);
            for (unsigned t = 0; t < 3; t++) {
                feed_terminal(cols, rows, t, s_phases[p].tm, s_phases[p].span);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            portENTER_CRITICAL(&stats_lock);
            g_fx_frame = 0;
            measured_cycles = 0;
            measured_count = measured_max = fast_bands = fallback_bands = 0;
            fused_bands = 0;
            portEXIT_CRITICAL(&stats_lock);
            for (unsigned t = 0; t < 12; t++) {
                feed_terminal(cols, rows, t + 3, s_phases[p].tm, s_phases[p].span);
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            portENTER_CRITICAL(&stats_lock);
            uint32_t n = measured_count, mx = measured_max;
            uint32_t avg = n ? measured_cycles / n : 0;
            uint32_t fast = fast_bands, fallback = fallback_bands;
            uint32_t fused_count = fused_bands;
            portEXIT_CRITICAL(&stats_lock);
            if (backend == 1 || backend == 3) {
                uint32_t before = measured_count;
                assert(scan8_guard_probe(output, (const uint8_t *)masks,
                            (const uint32_t (*)[4])masks,
                            (const uint32_t (*)[4])masks, masks, 100) == 0);
                assert(measured_count > before);
            }
            printf("PIPELINE font=%s phase=%s mode=%s run=%u avg=%lu max=%lu n=%lu fast=%lu fallback=%lu fused=%lu\n",
                   font_size_name(font_active_size()), s_phases[p].name,
                   backend == 3 ? "fused" : backend ? "simd" : "scalar", k,
                   (unsigned long)avg, (unsigned long)mx, (unsigned long)n,
                   (unsigned long)fast, (unsigned long)fallback, (unsigned long)fused_count);
        }
    }
    backend = 2;
    vTaskDelay(pdMS_TO_TICKS(100));
    portENTER_CRITICAL(&stats_lock);
    measured_cycles = 0;
    measured_count = measured_max = 0;
    portEXIT_CRITICAL(&stats_lock);
    vTaskDelay(pdMS_TO_TICKS(1200));
    portENTER_CRITICAL(&stats_lock);
    uint32_t n = measured_count, mx = measured_max;
    uint32_t avg = n ? measured_cycles / n : 0;
    portEXIT_CRITICAL(&stats_lock);
    printf("PIPELINE copy-only avg=%lu max=%lu n=%lu\n",
           (unsigned long)avg, (unsigned long)mx, (unsigned long)n);
    puts("PIPELINE live vector-context canaries PASS");
    printf("PIPELINE buffer alignment mask=%lx\n", (unsigned long)seen_alignments);
    puts("PIPELINE DONE");
    vTaskDelete(NULL);
}

void app_main(void)
{
    assert(xTaskCreatePinnedToCore(run, "pipeline_bench", 8192, NULL, 5, NULL, 1) == pdPASS);
}
