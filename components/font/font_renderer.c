/*
 * Font rendering implementation — compressed glyph table format ("v1").
 *
 * Glyph tables are no longer flat per-codepoint row arrays: each face has a
 * range/idx table (unchanged layout) whose idx entries are BYTE OFFSETS into
 * a compact "pool" of variable-length PackBits-encoded glyph records (see
 * terminus_font.h for the exact wire format). Rendering therefore always
 * goes through font_decode_glyph(), which expands a record into a plain
 * row buffer on every call — there is no cached pointer to a ready-made
 * glyph any more.
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
 * size leaves NULL `ranges`/`pool` — font_init() skips those when honouring
 * (or falling back from) the stored setting. Flash-resident; consulted only
 * during init, never from the ISR. Only POINTERS live here: the `const int`
 * / `const uint16_t` count and size objects are not constant expressions,
 * so they cannot sit in this static initializer — they are fetched via the
 * variant_*() switch functions below instead. */
typedef struct {
    uint8_t           width;
    uint8_t           height;
    const char       *name;
    const FontRange  *ranges;       /* NULL when this size is compiled out */
    const uint8_t    *pool;
    const uint16_t   *palette;      /* rb==2 sizes only; NULL for 8x16 */
    const FontRange  *bold_ranges;
    const uint8_t    *bold_pool;
} font_variant_t;

static const font_variant_t s_variants[FONT_SIZE_COUNT] = {
    [FONT_SIZE_8X16] = {
        .width = 8, .height = 16, .name = "8x16",
#if FONT_RT_8X16
        .ranges = terminus8x16_ranges,
        .pool   = terminus8x16_pool,
#if FONT_BOLD_ENABLED
        .bold_ranges = terminus8x16_bold_ranges,
        .bold_pool   = terminus8x16_bold_pool,
#endif
#endif
    },
    [FONT_SIZE_10X20] = {
        .width = 10, .height = 20, .name = "10x20",
#if FONT_RT_10X20
        .ranges  = terminus10x20_ranges,
        .pool    = terminus10x20_pool,
        .palette = terminus10x20_palette,
#if FONT_BOLD_ENABLED
        .bold_ranges = terminus10x20_bold_ranges,
        .bold_pool   = terminus10x20_bold_pool,
#endif
#endif
    },
    [FONT_SIZE_12X24] = {
        .width = 12, .height = 24, .name = "12x24",
#if FONT_RT_12X24
        .ranges  = terminus12x24_ranges,
        .pool    = terminus12x24_pool,
        .palette = terminus12x24_palette,
#if FONT_BOLD_ENABLED
        .bold_ranges = terminus12x24_bold_ranges,
        .bold_pool   = terminus12x24_bold_pool,
#endif
#endif
    },
};

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

static uint16_t variant_pool_bytes(font_size_t s)
{
    switch (s) {
#if FONT_RT_8X16
    case FONT_SIZE_8X16:  return terminus8x16_pool_bytes;
#endif
#if FONT_RT_10X20
    case FONT_SIZE_10X20: return terminus10x20_pool_bytes;
#endif
#if FONT_RT_12X24
    case FONT_SIZE_12X24: return terminus12x24_pool_bytes;
#endif
    default: return 0;
    }
}

static uint16_t variant_bold_pool_bytes(font_size_t s)
{
#if FONT_BOLD_ENABLED
    switch (s) {
#if FONT_RT_8X16
    case FONT_SIZE_8X16:  return terminus8x16_bold_pool_bytes;
#endif
#if FONT_RT_10X20
    case FONT_SIZE_10X20: return terminus10x20_bold_pool_bytes;
#endif
#if FONT_RT_12X24
    case FONT_SIZE_12X24: return terminus12x24_bold_pool_bytes;
#endif
    default: return 0;
    }
#else
    (void)s;
    return 0;
#endif
}

/* rb==2 sizes only (10x20, 12x24); 8x16 has no palette and falls to the
 * default case. */
static uint16_t variant_palette_len(font_size_t s)
{
    switch (s) {
#if FONT_RT_10X20
    case FONT_SIZE_10X20: return terminus10x20_palette_len;
#endif
#if FONT_RT_12X24
    case FONT_SIZE_12X24: return terminus12x24_palette_len;
#endif
    default: return 0;
    }
}

static uint8_t variant_bold_smear_left(font_size_t s)
{
#if FONT_BOLD_ENABLED
    switch (s) {
#if FONT_RT_8X16
    case FONT_SIZE_8X16:  return terminus8x16_bold_smear_left;
#endif
#if FONT_RT_10X20
    case FONT_SIZE_10X20: return terminus10x20_bold_smear_left;
#endif
#if FONT_RT_12X24
    case FONT_SIZE_12X24: return terminus12x24_bold_smear_left;
#endif
    default: return 0;
    }
#else
    (void)s;
    return 0;
#endif
}

/* DRAM-resident active state — must be readable from the ISR with the flash
 * cache disabled, so every field the decode path touches lives here. */
static DRAM_ATTR const FontRange *s_ranges         = NULL;
static DRAM_ATTR int              s_num_ranges     = 0;
static DRAM_ATTR const uint8_t   *s_pool           = NULL;

static DRAM_ATTR const FontRange *s_bold_ranges    = NULL;
static DRAM_ATTR int              s_num_bold       = 0;
static DRAM_ATTR const uint8_t   *s_bold_pool      = NULL;
static DRAM_ATTR uint8_t          s_bold_smear_left = 0;

static DRAM_ATTR const uint16_t  *s_palette        = NULL;
static DRAM_ATTR uint16_t         s_palette_len    = 0;

static DRAM_ATTR int              s_width          = 8;
static DRAM_ATTR int              s_height         = 16;
static DRAM_ATTR int              s_rb             = 1;   /* FONT_ROW_BYTES(s_width) */
static DRAM_ATTR size_t           s_glyph_bytes    = 16;

static font_size_t                s_active         = FONT_SIZE_8X16;

#ifndef BUILD_SIMULATOR
/* Copy one face's range table + idx arrays + pool into internal DRAM so the
 * bounce-buffer ISR can read it while the flash cache is disabled (NVS
 * writes). idx arrays are always uint16_t regardless of row width — they
 * hold pool byte offsets, not rows. Returns false when DRAM is exhausted
 * (caller keeps the flash-resident tables for this face). */
static bool load_face(const FontRange *src_ranges, int n,
                       const uint8_t *src_pool, uint16_t pool_bytes,
                       const FontRange **out_ranges, const uint8_t **out_pool)
{
    size_t idx_total = 0;
    for (int i = 0; i < n; i++)
        idx_total += (size_t)(src_ranges[i].last_char - src_ranges[i].first_char + 1)
                   * sizeof(uint16_t);

    uint8_t   *idx_buf   = heap_caps_malloc(idx_total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    FontRange *range_buf = heap_caps_malloc((size_t)n * sizeof(FontRange),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t   *pool_buf  = pool_bytes
        ? heap_caps_malloc(pool_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        : NULL;
    if (!idx_buf || !range_buf || (pool_bytes && !pool_buf)) {
        ESP_LOGE(TAG, "No DRAM for font face (%zu B idx + %u B pool)",
                 idx_total, (unsigned)pool_bytes);
        heap_caps_free(idx_buf);
        heap_caps_free(range_buf);
        heap_caps_free(pool_buf);
        return false;
    }

    uint8_t *dst = idx_buf;
    for (int i = 0; i < n; i++) {
        size_t nbytes = (size_t)(src_ranges[i].last_char - src_ranges[i].first_char + 1)
                       * sizeof(uint16_t);
        memcpy(dst, src_ranges[i].idx, nbytes);
        range_buf[i].first_char = src_ranges[i].first_char;
        range_buf[i].last_char  = src_ranges[i].last_char;
        range_buf[i].idx        = (const uint16_t *)dst;
        dst += nbytes;
    }
    if (pool_bytes)
        memcpy(pool_buf, src_pool, pool_bytes);

    ESP_LOGI(TAG, "Font face loaded: %d ranges, %zu B idx, %u B pool",
             n, idx_total, (unsigned)pool_bytes);
    *out_ranges = range_buf;
    *out_pool   = pool_buf;
    return true;
}

/* Palette is copied once per size and shared by the regular and bold face
 * (rb==2 sizes only). */
static bool load_palette(const uint16_t *src, uint16_t len, const uint16_t **out)
{
    if (!len) {
        *out = NULL;
        return true;
    }
    uint16_t *buf = heap_caps_malloc((size_t)len * sizeof(uint16_t),
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "No DRAM for font palette (%zu B)", (size_t)len * sizeof(uint16_t));
        return false;
    }
    memcpy(buf, src, (size_t)len * sizeof(uint16_t));
    ESP_LOGI(TAG, "Font palette loaded: %zu B", (size_t)len * sizeof(uint16_t));
    *out = buf;
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
size_t      font_glyph_bytes(void) { return s_glyph_bytes; }

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
    s_rb          = (int)FONT_ROW_BYTES(v->width);
    s_glyph_bytes = FONT_GLYPH_BYTES(v->width, v->height);

    const int      n_ranges        = variant_num_ranges(size);
    const int      n_bold          = variant_num_bold(size);
    const uint16_t pool_bytes      = variant_pool_bytes(size);
    const uint16_t bold_pool_bytes = variant_bold_pool_bytes(size);
    const uint16_t palette_len     = variant_palette_len(size);
    s_bold_smear_left = variant_bold_smear_left(size);

#ifndef BUILD_SIMULATOR
    /* Fall back to the flash-resident tables on DRAM exhaustion: renders
     * fine except while the flash cache is disabled (e.g. during NVS
     * writes). Better than a NULL dereference. */
    if (!load_face(v->ranges, n_ranges, v->pool, pool_bytes, &s_ranges, &s_pool)) {
        s_ranges = v->ranges;
        s_pool   = v->pool;
    }
    s_num_ranges = n_ranges;

#if FONT_BOLD_ENABLED
    if (v->bold_ranges) {
        if (!load_face(v->bold_ranges, n_bold, v->bold_pool, bold_pool_bytes,
                        &s_bold_ranges, &s_bold_pool)) {
            s_bold_ranges = v->bold_ranges;
            s_bold_pool   = v->bold_pool;
        }
        s_num_bold = n_bold;
    } else {
        s_bold_ranges = NULL;
        s_bold_pool   = NULL;
        s_num_bold    = 0;
    }
#endif

    if (v->palette) {
        if (!load_palette(v->palette, palette_len, &s_palette))
            s_palette = v->palette;
        s_palette_len = palette_len;
    } else {
        s_palette     = NULL;
        s_palette_len = 0;
    }

    ESP_LOGI(TAG, "Font %s ready: %d ranges/%u B pool, %d bold ranges/%u B pool, %u palette entries",
             v->name, s_num_ranges, (unsigned)pool_bytes, s_num_bold,
             (unsigned)bold_pool_bytes, (unsigned)palette_len);
#else
    /* Simulator: data already in normal RAM, no copy needed */
    s_ranges     = v->ranges;
    s_pool       = v->pool;
    s_num_ranges = n_ranges;
#if FONT_BOLD_ENABLED
    s_bold_ranges = v->bold_ranges;
    s_bold_pool   = v->bold_pool;
    s_num_bold    = n_bold;
#endif
    s_palette     = v->palette;
    s_palette_len = palette_len;
    ESP_LOGI(TAG, "Font system initialized (simulator, %s, %d ranges + %d bold, %u palette entries)",
             v->name, s_num_ranges, s_num_bold, (unsigned)palette_len);
#endif
}

/* ---- Decode path: IRAM_ATTR, no libc calls (flash cache may be disabled
 * when the bounce ISR runs). Everything below is reachable from
 * font_decode_glyph(). ---- */

/* Binary search shared by both faces; returns the pool BYTE OFFSET for `cp`
 * via *out_off. Byte arithmetic on idx is always uint16_t regardless of the
 * active row width. */
static IRAM_ATTR bool lookup_offset(const FontRange *ranges, int n, uint16_t cp,
                                     uint16_t *out_off)
{
    int lo = 0, hi = n - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if      (cp < ranges[mid].first_char) hi = mid - 1;
        else if (cp > ranges[mid].last_char)  lo = mid + 1;
        else {
            *out_off = ranges[mid].idx[cp - ranges[mid].first_char];
            return true;
        }
    }
    return false;
}

static IRAM_ATTR void zero_fill(void *out, size_t n)
{
    uint8_t *o = (uint8_t *)out;
    for (size_t i = 0; i < n; i++)
        o[i] = 0;
}

/* rb==2 symbol reader: u8 palette index, or the 0xFF escape followed by 2
 * raw row bytes (little-endian) inline in the pool stream. Advances *pp. */
static IRAM_ATTR uint16_t read_symbol16(const uint8_t **pp)
{
    const uint8_t *p = *pp;
    uint8_t idx = *p++;
    uint16_t val;
    if (idx == 0xFF) {
        uint16_t lo = *p++;
        uint16_t hi = *p++;
        val = (uint16_t)(lo | (hi << 8));
    } else {
        val = s_palette[idx];
    }
    *pp = p;
    return val;
}

/* Decode one glyph record at `pool + offset` into `out` (already
 * zero-filled by the caller). Header layout depends on the active cell
 * height: h==16 packs start/len into one byte, h>16 uses two. PackBits body
 * decodes exactly `len` rows starting at row `start`. */
static IRAM_ATTR void decode_record(const uint8_t *pool, uint16_t offset, void *out)
{
    const uint8_t *p = pool + offset;
    int start, len;

    if (s_height == 16) {
        uint8_t hdr = *p++;
        start = (hdr >> 4) & 0x0F;
        len   = (hdr & 0x0F) + 1;
    } else {
        start = *p++;
        len   = *p++;
        if (len == 0)
            return; /* blank glyph, no body — out is already all zero */
    }

    int row = start;
    int remaining = len;

    if (s_rb == 1) {
        uint8_t *rows = (uint8_t *)out;
        while (remaining > 0) {
            uint8_t c = *p++;
            if (c < 0x80) {
                int count = c + 1;
                for (int i = 0; i < count; i++)
                    rows[row++] = *p++;
                remaining -= count;
            } else {
                int count = (c & 0x7F) + 2;
                uint8_t sym = *p++;
                for (int i = 0; i < count; i++)
                    rows[row++] = sym;
                remaining -= count;
            }
        }
    } else {
        uint16_t *rows = (uint16_t *)out;
        while (remaining > 0) {
            uint8_t c = *p++;
            if (c < 0x80) {
                int count = c + 1;
                for (int i = 0; i < count; i++)
                    rows[row++] = read_symbol16(&p);
                remaining -= count;
            } else {
                int count = (c & 0x7F) + 2;
                uint16_t sym = read_symbol16(&p);
                for (int i = 0; i < count; i++)
                    rows[row++] = sym;
                remaining -= count;
            }
        }
    }
}

/* Regular-face decode: bsearch miss falls back to '?' (U+003F); if '?' is
 * itself unavailable, `out` is left all-zero (same guard against runaway
 * fallback as the old code's cp != 0x3F check — done here without actual
 * recursion since `out` is already zeroed either way). */
static IRAM_ATTR void decode_regular(uint16_t cp, void *out)
{
    zero_fill(out, s_glyph_bytes);

    uint16_t off;
    if (lookup_offset(s_ranges, s_num_ranges, cp, &off)) {
        decode_record(s_pool, off, out);
        return;
    }
    if (cp != 0x003F) {
        uint16_t qoff;
        if (lookup_offset(s_ranges, s_num_ranges, 0x003F, &qoff))
            decode_record(s_pool, qoff, out);
    }
    /* else: leave zero-filled */
}

#if FONT_BOLD_ENABLED
/* Smear one pixel in the face's smear direction, in place, per row. 8x16 is
 * the only LEFT-smearing (rb==1) size; 10x20/12x24 smear RIGHT (rb==2). The
 * per-variant s_bold_smear_left flag drives the choice, not rb, per spec. */
static IRAM_ATTR void smear_glyph(void *out)
{
    if (s_rb == 1) {
        uint8_t *rows = (uint8_t *)out;
        for (int r = 0; r < s_height; r++) {
            uint8_t v = rows[r];
            rows[r] = s_bold_smear_left ? (uint8_t)(v | ((v << 1) & 0xFF))
                                         : (uint8_t)(v | (v >> 1));
        }
    } else {
        uint16_t *rows = (uint16_t *)out;
        for (int r = 0; r < s_height; r++) {
            uint16_t v = rows[r];
            rows[r] = s_bold_smear_left ? (uint16_t)(v | ((v << 1) & 0xFFFF))
                                         : (uint16_t)(v | (v >> 1));
        }
    }
}
#endif

/**
 * Decode a glyph's bitmap into @p out — IRAM_ATTR, safe to call from the
 * ISR. See font.h for the contract.
 */
IRAM_ATTR void font_decode_glyph(uint16_t cp, bool bold, void *out)
{
#if FONT_BOLD_ENABLED
    if (bold) {
        uint16_t off;
        if (lookup_offset(s_bold_ranges, s_num_bold, cp, &off)) {
            if (off == 0xFFFF) {
                /* Synthesize: regular glyph, then smear — pixel-identical to
                 * the real Terminus bold per the generator's guarantee. */
                decode_regular(cp, out);
                smear_glyph(out);
            } else {
                /* Real bold (or stored-normal) exception record. */
                zero_fill(out, s_glyph_bytes);
                decode_record(s_bold_pool, off, out);
            }
            return;
        }
        /* Outside the bold subset: regular glyph, no smear. */
    }
#else
    (void)bold;
#endif
    decode_regular(cp, out);
}
