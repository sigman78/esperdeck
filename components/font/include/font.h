/*
 * Font rendering interface
 */

#ifndef FONT_H
#define FONT_H

#include <stdint.h>
#include "esp_attr.h"

#define FONT_WIDTH  8
#define FONT_HEIGHT 16

/**
 * Initialize font system
 */
void font_init(void);

/**
 * Get font glyph bitmap — IRAM_ATTR, safe to call from ISR.
 *
 * @param cp Unicode codepoint (BMP, U+0000..U+FFFF)
 * @return   Pointer to 16-byte glyph bitmap in DRAM, or fallback glyph
 */
const uint8_t* font_get_glyph(uint16_t cp);

#endif // FONT_H
