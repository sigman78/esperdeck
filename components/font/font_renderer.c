/*
 * Font renderer implementation
 */

#include "font.h"
#include "terminus_font.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <string.h>

#ifndef BUILD_SIMULATOR
#include "esp_heap_caps.h"
#endif

static const char *TAG = "font_renderer";

/* DRAM-resident pointers — must be accessible from ISR */
static DRAM_ATTR const FontRange *s_ranges         = NULL;
static DRAM_ATTR int              s_num_ranges     = 0;
static DRAM_ATTR const FontRange *s_bold_ranges    = NULL;
static DRAM_ATTR int              s_num_bold       = 0;

#ifndef BUILD_SIMULATOR
/* Copy a flash-resident range table + glyph data into internal DRAM so the
 * bounce-buffer ISR can read it while the flash cache is disabled (NVS
 * writes). Returns false when DRAM is exhausted (caller keeps flash). */
static bool load_ranges(const FontRange *src, int n,
                        const FontRange **out_ranges)
{
    size_t total = 0;
    for (int i = 0; i < n; i++)
        total += (size_t)(src[i].last_char - src[i].first_char + 1)
                 * FONT_GLYPH_BYTES;

    /* font_row_t-typed alloc keeps the row arrays naturally aligned. */
    font_row_t *data_buf = heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    FontRange *range_buf = heap_caps_malloc(
        (size_t)n * sizeof(FontRange), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!data_buf || !range_buf) {
        ESP_LOGE(TAG, "No DRAM for font table (%zu B)", total);
        heap_caps_free(data_buf);
        heap_caps_free(range_buf);
        return false;
    }

    font_row_t *dst = data_buf;
    for (int i = 0; i < n; i++) {
        size_t nglyphs = (size_t)(src[i].last_char - src[i].first_char + 1);
        memcpy(dst, src[i].data, nglyphs * FONT_GLYPH_BYTES);
        range_buf[i].first_char = src[i].first_char;
        range_buf[i].last_char  = src[i].last_char;
        range_buf[i].data       = dst;
        dst += nglyphs * FONT_HEIGHT;
    }
    ESP_LOGI(TAG, "Font table loaded: %d ranges, %zu bytes DRAM", n, total);
    *out_ranges = range_buf;
    return true;
}
#endif

/**
 * Initialize font system: copy glyph data from flash into DRAM heap.
 */
void font_init(void)
{
#ifndef BUILD_SIMULATOR
    /* Fall back to the flash-resident tables on DRAM exhaustion: renders
     * fine except while the flash cache is disabled (e.g. during NVS
     * writes). Better than a NULL dereference. */
    if (!load_ranges(terminus_ranges, terminus_num_ranges, &s_ranges))
        s_ranges = terminus_ranges;
    s_num_ranges = terminus_num_ranges;
#if FONT_BOLD_ENABLED
    if (!load_ranges(terminus_bold_ranges, terminus_num_bold_ranges,
                     &s_bold_ranges))
        s_bold_ranges = terminus_bold_ranges;
    s_num_bold = terminus_num_bold_ranges;
#endif
    ESP_LOGI(TAG, "Font %dx%d ready (%d ranges + %d bold)",
             FONT_WIDTH, FONT_HEIGHT, s_num_ranges, s_num_bold);
#else
    /* Simulator: data already in normal RAM, no copy needed */
    s_ranges     = terminus_ranges;
    s_num_ranges = terminus_num_ranges;
#if FONT_BOLD_ENABLED
    s_bold_ranges = terminus_bold_ranges;
    s_num_bold    = terminus_num_bold_ranges;
#endif
    ESP_LOGI(TAG, "Font system initialized (simulator, %dx%d, %d ranges + %d bold)",
             FONT_WIDTH, FONT_HEIGHT, s_num_ranges, s_num_bold);
#endif
}

/* Binary search shared by both faces. IRAM — called from the bounce ISR. */
static IRAM_ATTR const font_row_t *lookup(const FontRange *ranges, int n,
                                          uint16_t cp)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if      (cp < ranges[mid].first_char) hi = mid - 1;
        else if (cp > ranges[mid].last_char)  lo = mid + 1;
        else {
            return ranges[mid].data
                 + (size_t)(cp - ranges[mid].first_char) * FONT_HEIGHT;
        }
    }
    return NULL;
}

/**
 * Get font glyph bitmap — IRAM_ATTR so it is safe to call from the ISR.
 *
 * @param cp Unicode codepoint (BMP, U+0000..U+FFFF)
 * @return   Pointer to FONT_HEIGHT glyph rows in DRAM, or fallback glyph
 */
IRAM_ATTR const font_row_t *font_get_glyph(uint16_t cp)
{
    const font_row_t *g = lookup(s_ranges, s_num_ranges, cp);
    if (g) return g;
    /* fallback: U+003F '?' */
    if (cp != 0x003F) return font_get_glyph(0x003F);
    return NULL;
}

/**
 * Get the BOLD glyph — NULL when no stored bold form exists (identical to
 * normal, outside subset A, or bold disabled); callers use the normal glyph.
 */
IRAM_ATTR const font_row_t *font_get_glyph_bold(uint16_t cp)
{
    return lookup(s_bold_ranges, s_num_bold, cp);
}
