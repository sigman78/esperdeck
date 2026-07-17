#pragma once
#include <stdint.h>
#include <stddef.h>
#include "font.h"

#define FONT_GLYPH_BYTES ((size_t)FONT_HEIGHT * sizeof(font_row_t))

typedef struct {
    uint16_t first_char;
    uint16_t last_char;
    const font_row_t *data;  /* points into flash (or DRAM after init) */
} FontRange;

extern const FontRange terminus_ranges[];
extern const int       terminus_num_ranges;

#if FONT_BOLD_ENABLED
extern const FontRange terminus_bold_ranges[];
extern const int       terminus_num_bold_ranges;
#endif
