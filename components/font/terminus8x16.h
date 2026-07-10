#pragma once
#include <stdint.h>
#include <stddef.h>

#define FONT_GLYPH_BYTES 16   /* bytes per glyph (8 wide × 16 tall) */

typedef struct {
    uint16_t first_char;
    uint16_t last_char;
    const uint8_t *data;  /* points into flash (or DRAM after init) */
} FontRange;

extern const FontRange terminus_ranges[];
extern const int       terminus_num_ranges;
