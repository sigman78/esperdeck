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
 * reboot: the character grid, the DMA bounce-band height and the DRAM glyph
 * copy are all fixed during init, and esp_lcd captures the bounce geometry
 * once when the panel comes up.
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

/* Which sizes are linked into this build. An undefined FONT_RT_* evaluates
 * to 0 in #if, so only the enabled ones need defining. */
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
 * ISR's static column caches and bound the DMA bounce band, so they must
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
 * Copies the chosen size's glyph tables from flash into internal DRAM so the
 * bounce ISR can read them while the flash cache is disabled. Only the
 * SELECTED size is copied — linking three tables costs flash, not DRAM.
 *
 * @param size  Requested size. Falls back to the first linked size when the
 *              request is out of range or was compiled out (a stored setting
 *              can outlive a Kconfig change). The simulator ignores it.
 */
void font_init(font_size_t size);

/** Active size, and whether a given size is linked into this build. */
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
 * ISR. The tables are compressed (crop + PackBits row-RLE, see
 * terminus_font.h); this is the only way to obtain glyph rows.
 *
 * Writes font_glyph_bytes() bytes: font_height() rows of
 * FONT_ROW_BYTES(font_width()) bytes each (u16 rows assume @p out is
 * 2-aligned). Unknown codepoints decode the '?' fallback; if even that is
 * missing, all-zero rows.
 *
 * @param cp   Unicode codepoint (BMP, U+0000..U+FFFF)
 * @param bold Decode the bold form: a stored bold exception glyph, or the
 *             regular glyph smeared one pixel (pixel-identical to the real
 *             Terminus bold). Outside the bold subset — or with bold
 *             compiled out — decodes the regular glyph unchanged.
 * @param out  Destination for the decoded rows.
 */
void font_decode_glyph(uint16_t cp, bool bold, void *out);

#endif // FONT_H
