/*
 * Font rendering interface
 */

#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include "esp_attr.h"

/*
 * Compile-time font size selection.
 *
 * IDF builds:  Kconfig choice → CONFIG_CYBERDECK_FONT_{8X16,10X20,12X24}
 * Simulator:   cmake -DFONT_SIZE={8x16,10x20,12x24} → CYBERDECK_FONT_*
 *
 * The terminal grid follows from the cell size (800x480 panel):
 *   8x16 → 100x30    10x20 → 80x24    12x24 → 66x20 (8 px right margin)
 */
#if defined(CONFIG_CYBERDECK_FONT_12X24) || defined(CYBERDECK_FONT_12X24)
#define FONT_WIDTH  12
#define FONT_HEIGHT 24
#elif defined(CONFIG_CYBERDECK_FONT_10X20) || defined(CYBERDECK_FONT_10X20)
#define FONT_WIDTH  10
#define FONT_HEIGHT 20
#else
#define FONT_WIDTH  8
#define FONT_HEIGHT 16
#endif

/*
 * One glyph scanline. The glyph occupies the low FONT_WIDTH bits; the
 * leftmost pixel is bit (FONT_WIDTH-1). A glyph is FONT_HEIGHT rows.
 */
#if FONT_WIDTH <= 8
typedef uint8_t  font_row_t;
#else
typedef uint16_t font_row_t;
#endif

/**
 * Initialize font system
 */
void font_init(void);

/**
 * Get font glyph bitmap — IRAM_ATTR, safe to call from ISR.
 *
 * @param cp Unicode codepoint (BMP, U+0000..U+FFFF)
 * @return   Pointer to FONT_HEIGHT rows in DRAM, or fallback glyph
 */
const font_row_t* font_get_glyph(uint16_t cp);

#endif // FONT_H
