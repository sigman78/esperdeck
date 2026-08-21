/*
 * Font rendering implementation — compressed glyph table format ("v1").
 *
 * Glyph tables are not flat per-codepoint row arrays: each face has a
 * range/idx table whose idx entries are BYTE OFFSETS into a compact "pool"
 * of variable-length PackBits-encoded glyph records (see terminus_font.h
 * for the exact wire format). Rendering therefore always goes through
 * font_decode_glyph(), which expands a record into a plain row buffer on
 * every call — there is no cached pointer to a ready-made glyph.
 */

#include "font.h"
#include "terminus_font.h"
#include "esp_attr.h"
#include "esp_log.h"
#include <string.h>

/* The simulator provides an idfsim/esp_heap_caps.h stub, so this is
 * unconditional — the glyph cache is built on both targets. */
#include "esp_heap_caps.h"

static const char *TAG = "font_renderer";

/* Every size this build can select, in font_size_t order. A compiled-out
 * size leaves NULL faces — font_init() skips those when honouring (or
 * falling back from) the stored setting. Flash-resident; consulted only
 * during init, never from the ISR. */
typedef struct {
    uint8_t         width;
    uint8_t         height;
    const char     *name;
    const FontFace *regular;    /* NULL when this size is compiled out */
    const FontFace *bold;       /* NULL when bold is compiled out      */
} font_variant_t;

static const font_variant_t s_variants[FONT_SIZE_COUNT] = {
    [FONT_SIZE_8X16] = {
        .width = 8, .height = 16, .name = "8x16",
#if FONT_RT_8X16
        .regular = &terminus8x16_regular,
#if FONT_BOLD_ENABLED
        .bold    = &terminus8x16_bold,
#endif
#endif
    },
    [FONT_SIZE_10X20] = {
        .width = 10, .height = 20, .name = "10x20",
#if FONT_RT_10X20
        .regular = &terminus10x20_regular,
#if FONT_BOLD_ENABLED
        .bold    = &terminus10x20_bold,
#endif
#endif
    },
    [FONT_SIZE_12X24] = {
        .width = 12, .height = 24, .name = "12x24",
#if FONT_RT_12X24
        .regular = &terminus12x24_regular,
#if FONT_BOLD_ENABLED
        .bold    = &terminus12x24_bold,
#endif
#endif
    },
};

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

static DRAM_ATTR int              s_width          = 8;
static DRAM_ATTR int              s_height         = 16;
static DRAM_ATTR int              s_rb             = 1;   /* FONT_ROW_BYTES(s_width) */
static DRAM_ATTR size_t           s_glyph_bytes    = 16;

static font_size_t                s_active         = FONT_SIZE_8X16;

static void gcache_init(void);   /* decoded-glyph cache; see below */

#ifndef BUILD_SIMULATOR
/* Copy one face's range table + idx arrays + pool into internal DRAM so the
 * bounce-buffer ISR can read it while the flash cache is disabled (NVS
 * writes). idx arrays are always uint16_t regardless of row width — they
 * hold pool byte offsets, not rows. Returns false when DRAM is exhausted
 * (caller keeps the flash-resident tables for this face). */
static bool load_face(const FontFace *src,
                      const FontRange **out_ranges, const uint8_t **out_pool)
{
    const int n = src->num_ranges;
    size_t idx_total = 0;
    for (int i = 0; i < n; i++)
        idx_total += (size_t)(src->ranges[i].last_char - src->ranges[i].first_char + 1)
                   * sizeof(uint16_t);

    uint8_t   *idx_buf   = heap_caps_malloc(idx_total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    FontRange *range_buf = heap_caps_malloc((size_t)n * sizeof(FontRange),
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t   *pool_buf  = src->pool_bytes
        ? heap_caps_malloc(src->pool_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        : NULL;
    if (!idx_buf || !range_buf || (src->pool_bytes && !pool_buf)) {
        ESP_LOGE(TAG, "No DRAM for font face (%zu B idx + %u B pool)",
                 idx_total, (unsigned)src->pool_bytes);
        heap_caps_free(idx_buf);
        heap_caps_free(range_buf);
        heap_caps_free(pool_buf);
        return false;
    }

    uint8_t *dst = idx_buf;
    for (int i = 0; i < n; i++) {
        size_t nbytes = (size_t)(src->ranges[i].last_char - src->ranges[i].first_char + 1)
                       * sizeof(uint16_t);
        memcpy(dst, src->ranges[i].idx, nbytes);
        range_buf[i].first_char = src->ranges[i].first_char;
        range_buf[i].last_char  = src->ranges[i].last_char;
        range_buf[i].idx        = (const uint16_t *)dst;
        dst += nbytes;
    }
    if (src->pool_bytes)
        memcpy(pool_buf, src->pool, src->pool_bytes);

    ESP_LOGI(TAG, "Font face loaded: %d ranges, %zu B idx, %u B pool",
             n, idx_total, (unsigned)src->pool_bytes);
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
    return size >= 0 && size < FONT_SIZE_COUNT && s_variants[size].regular != NULL;
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

    const font_variant_t *v   = &s_variants[size];
    const FontFace *reg  = v->regular;
    const FontFace *bold = v->bold;
    s_active      = size;
    s_width       = v->width;
    s_height      = v->height;
    s_rb          = (int)FONT_ROW_BYTES(v->width);
    s_glyph_bytes = FONT_GLYPH_BYTES(v->width, v->height);
    s_bold_smear_left = bold ? bold->smear_left : 0;

#ifndef BUILD_SIMULATOR
    /* Fall back to the flash-resident tables on DRAM exhaustion: renders
     * fine except while the flash cache is disabled (e.g. during NVS
     * writes). Better than a NULL dereference. */
    if (!load_face(reg, &s_ranges, &s_pool)) {
        s_ranges = reg->ranges;
        s_pool   = reg->pool;
    }
    s_num_ranges = reg->num_ranges;

    if (bold) {
        if (!load_face(bold, &s_bold_ranges, &s_bold_pool)) {
            s_bold_ranges = bold->ranges;
            s_bold_pool   = bold->pool;
        }
        s_num_bold = bold->num_ranges;
    } else {
        s_bold_ranges = NULL;
        s_bold_pool   = NULL;
        s_num_bold    = 0;
    }

    if (reg->palette) {
        if (!load_palette(reg->palette, reg->palette_len, &s_palette))
            s_palette = reg->palette;
    } else {
        s_palette = NULL;
    }

    ESP_LOGI(TAG, "Font %s ready: %d ranges/%u B pool, %d bold ranges/%u B pool, %u palette entries",
             v->name, s_num_ranges, (unsigned)reg->pool_bytes, s_num_bold,
             (unsigned)(bold ? bold->pool_bytes : 0), (unsigned)reg->palette_len);
    gcache_init();
#else
    /* Simulator: data already in normal RAM, no copy needed */
    s_ranges      = reg->ranges;
    s_pool        = reg->pool;
    s_num_ranges  = reg->num_ranges;
    s_bold_ranges = bold ? bold->ranges : NULL;
    s_bold_pool   = bold ? bold->pool : NULL;
    s_num_bold    = bold ? bold->num_ranges : 0;
    s_palette     = reg->palette;
    ESP_LOGI(TAG, "Font system initialized (simulator, %s, %d ranges + %d bold, %u palette entries)",
             v->name, s_num_ranges, s_num_bold, (unsigned)reg->palette_len);
    gcache_init();
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

/* Runs before EVERY glyph decode, so its width matters. Measured on internal
 * SRAM: a byte store costs the same as a word store (~5 cyc either way — the
 * cost is per instruction, not per byte), so byte-at-a-time fills pay 4x.
 * Callers hand us the 4-aligned row cache with a 4-multiple glyph stride
 * (16 / 40 / 48 B), so the word path is the one that runs; the byte loop is
 * kept for the host tests, which pass arbitrary buffers. */
static IRAM_ATTR void zero_fill(void *out, size_t n)
{
    uint8_t *o = (uint8_t *)out;
    if ((((uintptr_t)o | n) & 3u) == 0) {
        uint32_t *w = (uint32_t *)out;
        const size_t nw = n >> 2;
        for (size_t i = 0; i < nw; i++)
            w[i] = 0;
        return;
    }
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
/* The smear is a per-row shift-or with no carry between rows, which makes it
 * exactly a SWAR operation: process 4 rows (rb==1) or 2 rows (rb==2) per
 * 32-bit word and mask off the bit that would cross a lane boundary.
 *   left : per lane (v << 1), so clear each lane's bit 0 after shifting
 *   right: per lane (v >> 1), so clear each lane's top bit after shifting
 * Bit-identical to the byte/halfword loop it replaces. Both the row cache and
 * the glyph stride are 4-aligned, so the word path always applies. */
static IRAM_ATTR void smear_glyph(void *out)
{
    uint32_t *w = (uint32_t *)out;
    const int nrows = s_height;
    const int rows_per_word = (s_rb == 1) ? 4 : 2;

    if ((((uintptr_t)out & 3u) == 0) && (nrows % rows_per_word) == 0) {
        const int nw = nrows / rows_per_word;
        if (s_rb == 1) {
            if (s_bold_smear_left)
                for (int i = 0; i < nw; i++) w[i] |= (w[i] << 1) & 0xFEFEFEFEu;
            else
                for (int i = 0; i < nw; i++) w[i] |= (w[i] >> 1) & 0x7F7F7F7Fu;
        } else {
            if (s_bold_smear_left)
                for (int i = 0; i < nw; i++) w[i] |= (w[i] << 1) & 0xFFFEFFFEu;
            else
                for (int i = 0; i < nw; i++) w[i] |= (w[i] >> 1) & 0x7FFF7FFFu;
        }
        return;
    }

    /* Fallback for a hypothetical odd geometry (no shipped size needs it). */
    if (s_rb == 1) {
        uint8_t *rows = (uint8_t *)out;
        for (int r = 0; r < nrows; r++) {
            uint8_t v = rows[r];
            rows[r] = s_bold_smear_left ? (uint8_t)(v | ((v << 1) & 0xFF))
                                        : (uint8_t)(v | (v >> 1));
        }
    } else {
        uint16_t *rows = (uint16_t *)out;
        for (int r = 0; r < nrows; r++) {
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
static IRAM_ATTR void decode_uncached(uint16_t cp, bool bold, void *out)
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

/*
 * Decoded-glyph cache — 2-way set associative, bounded.
 *
 * build_row_cache() decodes every visible cell on every frame: 3000 decodes
 * per frame at 100x30, ~146k/s at 48.8 Hz, over a distinct set far smaller
 * than the font (Terminus ships ~1470 glyphs; caching all of them would cost
 * 70 KB at 12x24). So: cache a bounded working set and evict.
 *
 * Sized at 256 entries. A realistic TUI screen — ASCII plus box drawing,
 * blocks and accents — is roughly 130-190 distinct glyphs including bold, so
 * 256 leaves headroom without thrashing. Overflow is graceful rather than
 * cliff-edged: a set that overflows costs one decode for the loser, not a
 * cascade, and there are 3000 cells to amortise it over.
 *
 * 2-way, not direct-mapped: with ~150 items in 256 direct-mapped slots the
 * birthday bound says ~44% of them would share a slot with something else.
 * Two ways drops the overflow probability (3+ items landing in one set) to
 * ~12% of sets. 4-way would be better still but costs more tag compares on
 * every hit, and 2-way already puts the miss cost under 1% of the band.
 *
 * Replacement is round-robin per set, flipped only when a line is FILLED.
 * True LRU would need a recency write on every hit — 3000 stores per frame in
 * ISR context — to improve a case that is already under 1%. Not worth it.
 *
 * Filled lazily; the first frame warms it. Internal DRAM: the bounce ISR reads
 * this with the flash cache disabled.
 */
#define GC_SETS_LOG 7u
#define GC_SETS     (1u << GC_SETS_LOG)          /* 128 */
#define GC_WAYS     2u
#define GC_ENT      (GC_SETS * GC_WAYS)          /* 256 */
#define GC_EMPTY    0xFFFFFFFFu                  /* keys are <= 0x1FFFF */

static DRAM_ATTR uint32_t *s_gc_tag;             /* [GC_ENT]                  */
static DRAM_ATTR uint8_t  *s_gc_bits;            /* [GC_SETS/8] victim bits   */
static DRAM_ATTR uint8_t  *s_gc_data;            /* [GC_ENT][s_glyph_bytes]   */
static uint8_t            *s_gc_owned;           /* single free()able block   */

/* Round-robin victim, advanced only on fill so hits stay read-only. */
static IRAM_ATTR unsigned gc_victim(unsigned set)
{
    const unsigned byte = set >> 3, bit = 1u << (set & 7u);
    const unsigned w = (s_gc_bits[byte] & bit) ? 1u : 0u;
    s_gc_bits[byte] ^= (uint8_t)bit;
    return w;
}

static IRAM_ATTR unsigned gc_fill(unsigned set, uint32_t key,
                                  uint16_t cp, bool bold)
{
    uint32_t *const tg = s_gc_tag + set * GC_WAYS;
    unsigned way;
    if      (tg[0] == GC_EMPTY) way = 0;         /* warm-up: take a free way */
    else if (tg[1] == GC_EMPTY) way = 1;
    else                        way = gc_victim(set);
    decode_uncached(cp, bold,
                    s_gc_data + (size_t)(set * GC_WAYS + way) * s_glyph_bytes);
    tg[way] = key;
    return way;
}

IRAM_ATTR void font_decode_glyph(uint16_t cp, bool bold, void *out)
{
    if (!s_gc_tag) {
        decode_uncached(cp, bold, out);
        return;
    }
    const uint32_t key = (uint32_t)cp | (bold ? 0x10000u : 0u);
    const unsigned set = (unsigned)((key * 2654435761u) >> (32u - GC_SETS_LOG));
    uint32_t *const tg = s_gc_tag + set * GC_WAYS;

    unsigned way;
    if      (tg[0] == key) way = 0;
    else if (tg[1] == key) way = 1;
    else                   way = gc_fill(set, key, cp, bold);

    const uint32_t *src = (const uint32_t *)(s_gc_data +
        (size_t)(set * GC_WAYS + way) * s_glyph_bytes);
    uint32_t *dst = (uint32_t *)out;
    const size_t nw = s_glyph_bytes >> 2;        /* 4 / 10 / 12 words */
    for (size_t w = 0; w < nw; w++)
        dst[w] = src[w];
}

static void gcache_init(void)
{
    /* font_init() is once-per-boot today (a size change needs a reboot), but
     * do not leave a stale cache behind if that ever changes. */
    s_gc_tag = NULL; s_gc_bits = NULL; s_gc_data = NULL;
    heap_caps_free(s_gc_owned);
    s_gc_owned = NULL;

    /* The hit path copies whole words; every shipped stride is 16/40/48 B. */
    if (s_glyph_bytes & 3u) {
        ESP_LOGW(TAG, "glyph stride %u not word-multiple - cache disabled",
                 (unsigned)s_glyph_bytes);
        return;
    }

    const size_t tag_b  = (size_t)GC_ENT * sizeof(uint32_t);
    const size_t bits_b = GC_SETS / 8u;
    const size_t data_b = (size_t)GC_ENT * s_glyph_bytes;
    uint8_t *buf = heap_caps_malloc(tag_b + bits_b + data_b,
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGW(TAG, "No DRAM for glyph cache (%u B) - decoding per frame",
                 (unsigned)(tag_b + bits_b + data_b));
        return;
    }
    s_gc_owned = buf;
    s_gc_tag   = (uint32_t *)buf;                /* 4-aligned by malloc */
    s_gc_bits  = buf + tag_b;
    s_gc_data  = buf + tag_b + bits_b;           /* tag_b+bits_b = 1040, 4-aligned */

    for (unsigned i = 0; i < GC_ENT; i++) s_gc_tag[i] = GC_EMPTY;
    for (unsigned i = 0; i < bits_b; i++) s_gc_bits[i] = 0;

    ESP_LOGI(TAG, "Glyph cache: %u B, %u sets x %u ways x %u B",
             (unsigned)(tag_b + bits_b + data_b), (unsigned)GC_SETS,
             (unsigned)GC_WAYS, (unsigned)s_glyph_bytes);
}
