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
static DRAM_ATTR const FontRange *s_ranges    = NULL;
static DRAM_ATTR int              s_num_ranges = 0;

/**
 * Initialize font system: copy glyph data from flash into DRAM heap.
 */
void font_init(void)
{
#ifndef BUILD_SIMULATOR
    size_t total = 0;
    for (int i = 0; i < terminus_num_ranges; i++)
        total += (size_t)(terminus_ranges[i].last_char - terminus_ranges[i].first_char + 1)
                 * FONT_GLYPH_BYTES;

    /* font_row_t-typed alloc keeps the row arrays naturally aligned. */
    font_row_t *data_buf = heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    FontRange *range_buf = heap_caps_malloc(
        (size_t)terminus_num_ranges * sizeof(FontRange), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!data_buf || !range_buf) {
        /* Fall back to flash-resident tables: renders fine except while the
         * flash cache is disabled (e.g. during NVS writes). Better than a
         * NULL dereference. */
        ESP_LOGE(TAG, "No DRAM for font (%zu B) — using flash tables", total);
        heap_caps_free(data_buf);
        heap_caps_free(range_buf);
        s_ranges     = terminus_ranges;
        s_num_ranges = terminus_num_ranges;
        return;
    }

    font_row_t *dst = data_buf;
    for (int i = 0; i < terminus_num_ranges; i++) {
        size_t nglyphs = (size_t)(terminus_ranges[i].last_char
                                  - terminus_ranges[i].first_char + 1);
        memcpy(dst, terminus_ranges[i].data, nglyphs * FONT_GLYPH_BYTES);
        range_buf[i].first_char = terminus_ranges[i].first_char;
        range_buf[i].last_char  = terminus_ranges[i].last_char;
        range_buf[i].data       = dst;
        dst += nglyphs * FONT_HEIGHT;
    }
    s_ranges     = range_buf;
    s_num_ranges = terminus_num_ranges;
    ESP_LOGI(TAG, "Font %dx%d loaded: %u ranges, %zu bytes DRAM",
             FONT_WIDTH, FONT_HEIGHT, terminus_num_ranges, total);
#else
    /* Simulator: data already in normal RAM, no copy needed */
    s_ranges     = terminus_ranges;
    s_num_ranges = terminus_num_ranges;
    ESP_LOGI(TAG, "Font system initialized (simulator, %dx%d, %d ranges)",
             FONT_WIDTH, FONT_HEIGHT, s_num_ranges);
#endif
}

/**
 * Get font glyph bitmap — IRAM_ATTR so it is safe to call from the ISR.
 *
 * @param cp Unicode codepoint (BMP, U+0000..U+FFFF)
 * @return   Pointer to FONT_HEIGHT glyph rows in DRAM, or fallback glyph
 */
IRAM_ATTR const font_row_t *font_get_glyph(uint16_t cp)
{
    int lo = 0, hi = s_num_ranges - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if      (cp < s_ranges[mid].first_char) hi = mid - 1;
        else if (cp > s_ranges[mid].last_char)  lo = mid + 1;
        else {
            return s_ranges[mid].data
                 + (size_t)(cp - s_ranges[mid].first_char) * FONT_HEIGHT;
        }
    }
    /* fallback: U+003F '?' */
    if (cp != 0x003F) return font_get_glyph(0x003F);
    return NULL;
}
