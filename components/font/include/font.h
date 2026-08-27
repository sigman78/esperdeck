/*
 * Font rendering interface
 */

#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_attr.h"

/*
 * Font size selection.
 *
 * IDF builds link EVERY size enabled in Kconfig (CYBERDECK_FONT_RT_*) and
 * choose one at boot from the stored setting. Changing the size needs a
 * reboot. The character grid, the DMA bounce-band height, and the DRAM
 * glyph copy are all fixed during init. esp_lcd captures the bounce
 * geometry once, when the panel comes up.
 *
 * The simulator and the unit tests stay SINGLE-size and compile-time
 * (cmake -DFONT_SIZE={8x16,10x20,12x24}) — neither has a settings store or a
 * reboot, and the tests want a fixed grid.
 *
 * The grid follows from the cell size on the 800x480 panel:
 *   8x16 -> 100x30    10x20 -> 80x24    12x24 -> 66x20 (8 px right margin)
 */
typedef enum {
    FONT_SIZE_8X16 = 0,
    FONT_SIZE_10X20,
    FONT_SIZE_12X24,
    FONT_SIZE_COUNT,
} font_size_t;

/* Which sizes this build links in. An undefined FONT_RT_* evaluates to 0
 * in #if, so only the enabled ones need defining. */
#ifdef BUILD_SIMULATOR
  #if defined(CYBERDECK_FONT_12X24)
    #define FONT_RT_12X24 1
  #elif defined(CYBERDECK_FONT_10X20)
    #define FONT_RT_10X20 1
  #else
    #define FONT_RT_8X16 1
  #endif
#else
  #ifdef CONFIG_CYBERDECK_FONT_RT_8X16
    #define FONT_RT_8X16 1
  #endif
  #ifdef CONFIG_CYBERDECK_FONT_RT_10X20
    #define FONT_RT_10X20 1
  #endif
  #ifdef CONFIG_CYBERDECK_FONT_RT_12X24
    #define FONT_RT_12X24 1
  #endif
#endif

#if !FONT_RT_8X16 && !FONT_RT_10X20 && !FONT_RT_12X24
#error "font: no size enabled — select at least one CYBERDECK_FONT_RT_*"
#endif

/*
 * Compile-time bounds over every size this build can select. They size the
 * ISR's static column caches and bound the DMA bounce band. They must
 * cover the widest grid (8x16 -> 100 cols) and the tallest band (16).
 */
#define FONT_MAX_COLS   100   /* 800 / 8  */
#define FONT_MAX_ROWS   30    /* 480 / 16 */
#define FONT_MAX_BAND   16    /* 8x16 renders full-row bands; taller ones half */

/* Bytes per glyph row / per whole (decoded) glyph, for a given cell size. */
#define FONT_ROW_BYTES(w)         ((size_t)((w) <= 8 ? 1 : 2))
#define FONT_GLYPH_BYTES(w, h)    ((size_t)(h) * FONT_ROW_BYTES(w))

/*
 * Renderer row-cache size: max over the ENABLED sizes of cols x glyph_bytes
 * (8x16: 100x16, 10x20: 80x40, 12x24: 66x48). The display ISR decodes each
 * column's glyph into this cache once per character row; the band scan loop
 * reads decoded rows with plain indexing.
 */
#if FONT_RT_10X20
#define FONT_MAX_CACHE_BYTES  3200
#elif FONT_RT_12X24
#define FONT_MAX_CACHE_BYTES  3168
#else
#define FONT_MAX_CACHE_BYTES  1600
#endif

/* Largest decoded glyph any ENABLED size produces — sizes static sprite
 * slot storage. */
#if FONT_RT_12X24
#define FONT_MAX_GLYPH_BYTES  48
#elif FONT_RT_10X20
#define FONT_MAX_GLYPH_BYTES  40
#else
#define FONT_MAX_GLYPH_BYTES  16
#endif

/*
 * Real bold glyphs (Terminus bold face, sparse subset A: ASCII,
 * Latin-1/Extended-A, Cyrillic). Device builds can opt out via Kconfig to
 * reclaim DRAM; the simulator always compiles it.
 */
#if defined(CONFIG_CYBERDECK_FONT_BOLD) || defined(BUILD_SIMULATOR)
#define FONT_BOLD_ENABLED 1
#else
#define FONT_BOLD_ENABLED 0
#endif

/**
 * Initialize the font system and select the active size.
 *
 * Copies the chosen size's glyph tables from flash into internal DRAM.
 * This lets the bounce ISR read them while NVS writes disable the flash
 * cache. This copies only the SELECTED size — linking three tables costs
 * flash, not DRAM.
 *
 * @param size  Requested size. Falls back to the first linked size when
 *              the request is out of range or the build excludes it. A
 *              stored setting can outlive a Kconfig change. The simulator
 *              ignores it.
 */
void font_init(font_size_t size);

/** Active size, and whether this build links a given size. */
font_size_t font_active_size(void);
bool        font_size_available(font_size_t size);

/** Short label for menus/logs, e.g. "8x16". Safe for any enum value. */
const char *font_size_name(font_size_t size);

/** Cell geometry of the active font — valid after font_init(). */
int font_width(void);
int font_height(void);

/** Decoded glyph size of the active font: font_height() rows of
 *  FONT_ROW_BYTES(font_width()) bytes. Valid after font_init(). */
size_t font_glyph_bytes(void);

/**
 * Decode a glyph's bitmap into @p out — IRAM_ATTR, safe to call from the
 * ISR. terminus_font.h documents the compressed format (crop + PackBits
 * row-RLE); this function is the only way to obtain glyph rows.
 *
 * Writes font_glyph_bytes() bytes: font_height() rows of
 * FONT_ROW_BYTES(font_width()) bytes each (u16 rows assume @p out is
 * 2-aligned). Unknown codepoints decode the '?' fallback; if even that is
 * missing, all-zero rows.
 *
 * @param cp   Unicode codepoint (BMP, U+0000..U+FFFF).
 * @param bold Decode the bold form: a stored bold exception glyph, or the
 *             regular glyph smeared one pixel. The smear is pixel-identical
 *             to the real Terminus bold. Outside the bold subset, or with
 *             bold compiled out, this decodes the regular glyph unchanged.
 * @param out  Destination for the decoded rows.
 */
void font_decode_glyph(uint16_t cp, bool bold, void *out);

/*
 * Dynamic sprite glyphs — RAM-backed codepoints for pixel-level UI.
 *
 * FONT_SPRITE_COUNT Private Use Area codepoints (U+E000..) decode from a
 * writable DRAM table instead of the Terminus pool. The renderer's row
 * cache rebuilds every character row every frame. A sprite updated from
 * any task is on screen within one frame — sub-cell animation, pixel-smooth
 * progress/slider edges and custom icons at zero per-frame app cost.
 *
 * ALL cells showing a codepoint share its slot: N visually distinct
 * simultaneous appearances need N slots. Screens author content for the
 * ACTIVE size (a size change reboots, so slots have boot lifetime); an
 * unset slot decodes all-zero (blank). Bold renders the same bitmap — pixel
 * art is not smeared. ONE writer per slot at a time: concurrent
 * font_sprite_set on the same slot interleaves buffer fills and publishes
 * garbage.
 *
 * The range is a LOCAL UI namespace: every render layer resolves these
 * codepoints, so remote/terminal text containing U+E000.. shows the current
 * slot content (blank when unset), not the '?' fallback. Screens that own
 * slots should font_sprite_clear_all() when handing the display to remote
 * content; sanitizing the range at vterm ingest is the eventual fix.
 *
 * Perf contract: the check lives in the decoded-glyph cache's MISS path, so
 * normal glyph decodes pay nothing. A displayed sprite costs a normal cache
 * hit. font_sprite_set() invalidates the slot's cache keys, so the next
 * frame refills. A per-slot generation counter closes the cross-core race
 * where a cache fill spans the whole update. Set/clear are task-context
 * calls (double-buffered against the concurrently reading ISR).
 */
#define FONT_SPRITE_BASE   0xE000u
/* Grow as takers appear. Cost is linear: 2 x FONT_MAX_GLYPH_BYTES DRAM
 * each. */
#define FONT_SPRITE_COUNT  8u

/** Codepoint of sprite slot @p slot (valid for slot < FONT_SPRITE_COUNT). */
static inline uint16_t font_sprite_cp(unsigned slot)
{
    return (uint16_t)(FONT_SPRITE_BASE + slot);
}

/**
 * Set sprite @p slot's bitmap: font_glyph_bytes() bytes in decoded-glyph
 * layout — font_height() rows of FONT_ROW_BYTES(font_width()) bytes, u16
 * rows little-endian, bit (width-1) = leftmost pixel. This call ignores
 * out-of-range slots. The sprite becomes visible next frame.
 */
void font_sprite_set(unsigned slot, const void *rows);

/**
 * Set sprite @p slot from font_height() uint16 row values (bit width-1 =
 * leftmost pixel) — packs into the active size's row layout internally, so
 * callers never touch the decoded-glyph byte format.
 */
void font_sprite_set_rows16(unsigned slot, const uint16_t *rows);

/** Reset sprite @p slot to blank (all-zero rows). */
void font_sprite_clear(unsigned slot);

/** Blank every slot — call when handing the display to remote content. */
void font_sprite_clear_all(void);

#endif // FONT_H
