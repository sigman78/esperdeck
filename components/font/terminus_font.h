#pragma once
#include <stdint.h>
#include <stddef.h>
#include "font.h"

/*
 * A glyph row is uint8_t at 8x16 but uint16_t at 10x20/12x24, and every
 * enabled size is linked at once, so the range table cannot name the row
 * type: `data` is void * and the renderer variant — instantiated per width,
 * so it knows the type statically — casts it back.
 */
typedef struct {
    uint16_t first_char;
    uint16_t last_char;
    const void *data;    /* points into flash (or DRAM after font_init) */
} FontRange;

/* Bytes per glyph row / per whole glyph, for a given cell size. */
#define FONT_ROW_BYTES(w)         ((size_t)((w) <= 8 ? 1 : 2))
#define FONT_GLYPH_BYTES(w, h)    ((size_t)(h) * FONT_ROW_BYTES(w))

#if FONT_RT_8X16
extern const FontRange terminus8x16_ranges[];
extern const int       terminus8x16_num_ranges;
#if FONT_BOLD_ENABLED
extern const FontRange terminus8x16_bold_ranges[];
extern const int       terminus8x16_num_bold_ranges;
#endif
#endif

#if FONT_RT_10X20
extern const FontRange terminus10x20_ranges[];
extern const int       terminus10x20_num_ranges;
#if FONT_BOLD_ENABLED
extern const FontRange terminus10x20_bold_ranges[];
extern const int       terminus10x20_num_bold_ranges;
#endif
#endif

#if FONT_RT_12X24
extern const FontRange terminus12x24_ranges[];
extern const int       terminus12x24_num_ranges;
#if FONT_BOLD_ENABLED
extern const FontRange terminus12x24_bold_ranges[];
extern const int       terminus12x24_num_bold_ranges;
#endif
#endif
