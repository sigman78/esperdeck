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

/* Every size this build can select, in font_size_t order. A compiled-out
 * size leaves a NULL `ranges` entry — font_init() skips those when honouring
 * (or falling back from) the stored setting. Flash-resident; consulted only
 * during init, never from the ISR. */
typedef struct {
    uint8_t           width;
    uint8_t           height;
    const char       *name;
    const FontRange  *ranges;       /* NULL when this size is compiled out */
    const FontRange  *bold_ranges;
} font_variant_t;

static const font_variant_t s_variants[FONT_SIZE_COUNT] = {
    [FONT_SIZE_8X16] = {
        .width = 8, .height = 16, .name = "8x16",
#if FONT_RT_8X16
        .ranges = terminus8x16_ranges,
#if FONT_BOLD_ENABLED
        .bold_ranges = terminus8x16_bold_ranges,
#endif
#endif
    },
    [FONT_SIZE_10X20] = {
        .width = 10, .height = 20, .name = "10x20",
#if FONT_RT_10X20
        .ranges = terminus10x20_ranges,
#if FONT_BOLD_ENABLED
        .bold_ranges = terminus10x20_bold_ranges,
#endif
#endif
    },
    [FONT_SIZE_12X24] = {
        .width = 12, .height = 24, .name = "12x24",
#if FONT_RT_12X24
        .ranges = terminus12x24_ranges,
#if FONT_BOLD_ENABLED
        .bold_ranges = terminus12x24_bold_ranges,
#endif
#endif
    },
};

/* The range COUNTS are `const int` objects, not constant expressions, so
 * they cannot sit in the initialiser above — fetched here instead. */
static int variant_num_ranges(font_size_t s)
{
    switch (s) {
#if FONT_RT_8X16
    case FONT_SIZE_8X16:  return terminus8x16_num_ranges;
#endif
#if FONT_RT_10X20
    case FONT_SIZE_10X20: return terminus10x20_num_ranges;
#endif
#if FONT_RT_12X24
    case FONT_SIZE_12X24: return terminus12x24_num_ranges;
#endif
    default: return 0;
    }
}

static int variant_num_bold(font_size_t s)
{
#if FONT_BOLD_ENABLED
    switch (s) {
#if FONT_RT_8X16
    case FONT_SIZE_8X16:  return terminus8x16_num_bold_ranges;
#endif
#if FONT_RT_10X20
    case FONT_SIZE_10X20: return terminus10x20_num_bold_ranges;
#endif
#if FONT_RT_12X24
    case FONT_SIZE_12X24: return terminus12x24_num_bold_ranges;
#endif
    default: return 0;
    }
#else
    (void)s;
    return 0;
#endif
}

/* DRAM-resident active state — must be readable from the ISR with the flash
 * cache disabled, so every field the lookup touches lives here. */
static DRAM_ATTR const FontRange *s_ranges      = NULL;
static DRAM_ATTR int              s_num_ranges  = 0;
static DRAM_ATTR const FontRange *s_bold_ranges = NULL;
static DRAM_ATTR int              s_num_bold    = 0;
static DRAM_ATTR int              s_width       = 8;
static DRAM_ATTR int              s_height      = 16;
static DRAM_ATTR size_t           s_glyph_bytes = 16;
static font_size_t                s_active      = FONT_SIZE_8X16;

#ifndef BUILD_SIMULATOR
/* Copy a flash-resident range table + glyph data into internal DRAM so the
 * bounce-buffer ISR can read it while the flash cache is disabled (NVS
 * writes). Returns false when DRAM is exhausted (caller keeps flash). */
static bool load_ranges(const FontRange *src, int n, size_t glyph_bytes,
                        const FontRange **out_ranges)
{
    size_t total = 0;
    for (int i = 0; i < n; i++)
        total += (size_t)(src[i].last_char - src[i].first_char + 1) * glyph_bytes;

    /* uint16_t-aligned alloc keeps the wider row arrays naturally aligned. */
    uint8_t *data_buf = heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    FontRange *range_buf = heap_caps_malloc(
        (size_t)n * sizeof(FontRange), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!data_buf || !range_buf) {
        ESP_LOGE(TAG, "No DRAM for font table (%zu B)", total);
        heap_caps_free(data_buf);
        heap_caps_free(range_buf);
        return false;
    }

    uint8_t *dst = data_buf;
    for (int i = 0; i < n; i++) {
        size_t nbytes = (size_t)(src[i].last_char - src[i].first_char + 1) * glyph_bytes;
        memcpy(dst, src[i].data, nbytes);
        range_buf[i].first_char = src[i].first_char;
        range_buf[i].last_char  = src[i].last_char;
        range_buf[i].data       = dst;
        dst += nbytes;
    }
    ESP_LOGI(TAG, "Font table loaded: %d ranges, %zu bytes DRAM", n, total);
    *out_ranges = range_buf;
    return true;
}
#endif

bool font_size_available(font_size_t size)
{
    return size >= 0 && size < FONT_SIZE_COUNT && s_variants[size].ranges != NULL;
}

const char *font_size_name(font_size_t size)
{
    if (size < 0 || size >= FONT_SIZE_COUNT) return "?";
    return s_variants[size].name;
}

font_size_t font_active_size(void) { return s_active; }
int         font_width(void)       { return s_width;  }
int         font_height(void)      { return s_height; }

/**
 * Initialize the font system: select a size and copy its glyph data from
 * flash into DRAM.
 */
void font_init(font_size_t size)
{
    /* A stored setting can outlive the Kconfig that provided its size, so an
     * unavailable request falls back to the first size still linked in
     * rather than leaving the renderer without a table. */
    if (!font_size_available(size)) {
        font_size_t fallback = FONT_SIZE_COUNT;
        for (int i = 0; i < FONT_SIZE_COUNT; i++) {
            if (font_size_available((font_size_t)i)) { fallback = (font_size_t)i; break; }
        }
        if (fallback == FONT_SIZE_COUNT) {
            ESP_LOGE(TAG, "no font size linked into this build");
            return;
        }
        if (size >= 0 && size < FONT_SIZE_COUNT)
            ESP_LOGW(TAG, "font %s not in this build — using %s",
                     s_variants[size].name, s_variants[fallback].name);
        size = fallback;
    }

    const font_variant_t *v = &s_variants[size];
    s_active      = size;
    s_width       = v->width;
    s_height      = v->height;
    s_glyph_bytes = FONT_GLYPH_BYTES(v->width, v->height);

    const int n_ranges = variant_num_ranges(size);
    const int n_bold   = variant_num_bold(size);

#ifndef BUILD_SIMULATOR
    /* Fall back to the flash-resident tables on DRAM exhaustion: renders
     * fine except while the flash cache is disabled (e.g. during NVS
     * writes). Better than a NULL dereference. */
    if (!load_ranges(v->ranges, n_ranges, s_glyph_bytes, &s_ranges))
        s_ranges = v->ranges;
    s_num_ranges = n_ranges;
#if FONT_BOLD_ENABLED
    if (v->bold_ranges &&
        !load_ranges(v->bold_ranges, n_bold, s_glyph_bytes, &s_bold_ranges))
        s_bold_ranges = v->bold_ranges;
    s_num_bold = s_bold_ranges ? n_bold : 0;
#endif
    ESP_LOGI(TAG, "Font %s ready (%d ranges + %d bold)",
             v->name, s_num_ranges, s_num_bold);
#else
    /* Simulator: data already in normal RAM, no copy needed */
    s_ranges     = v->ranges;
    s_num_ranges = n_ranges;
#if FONT_BOLD_ENABLED
    s_bold_ranges = v->bold_ranges;
    s_num_bold    = n_bold;
#endif
    ESP_LOGI(TAG, "Font system initialized (simulator, %s, %d ranges + %d bold)",
             v->name, s_num_ranges, s_num_bold);
#endif
}

/* Binary search shared by both faces. IRAM — called from the bounce ISR.
 * Byte arithmetic: the row type varies with the active size. */
static IRAM_ATTR const void *lookup(const FontRange *ranges, int n, uint16_t cp)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if      (cp < ranges[mid].first_char) hi = mid - 1;
        else if (cp > ranges[mid].last_char)  lo = mid + 1;
        else {
            return (const uint8_t *)ranges[mid].data
                 + (size_t)(cp - ranges[mid].first_char) * s_glyph_bytes;
        }
    }
    return NULL;
}

/**
 * Get font glyph bitmap — IRAM_ATTR so it is safe to call from the ISR.
 *
 * @param cp Unicode codepoint (BMP, U+0000..U+FFFF)
 * @return   Pointer to font_height() glyph rows in DRAM, or fallback glyph
 */
IRAM_ATTR const void *font_get_glyph(uint16_t cp)
{
    const void *g = lookup(s_ranges, s_num_ranges, cp);
    if (g) return g;
    /* fallback: U+003F '?' */
    if (cp != 0x003F) return font_get_glyph(0x003F);
    return NULL;
}

/**
 * Get the BOLD glyph. The bold face is a sparse subset of the regular one;
 * a codepoint without a stored bold form (identical to normal, outside the
 * subset, or bold disabled) falls back to the regular glyph.
 */
IRAM_ATTR const void *font_get_glyph_bold(uint16_t cp)
{
    const void *g = lookup(s_bold_ranges, s_num_bold, cp);
    return g ? g : font_get_glyph(cp);
}
