#pragma once
#include <stdint.h>
#include <stddef.h>
#include "font.h"

/*
 * Compressed glyph table format ("v1") — encoder in tools/gen_terminus.py,
 * which also generates the committed terminusWxH.c tables from the upstream
 * Terminus BDF release.
 *
 * Per face (regular / bold) of each size:
 *   ranges[] — sorted, non-overlapping codepoint ranges; idx holds one
 *              uint16_t per covered codepoint: the BYTE offset of the
 *              glyph record inside pool. 0xFFFF (bold faces only) means
 *              "synthesize: decode the regular glyph, then smear one pixel
 *              in the face's smear direction". Codepoints absorbed by range
 *              merging point at the '?' record — no sentinel needed.
 *   pool     — variable-length glyph records, <= 65534 bytes:
 *              header  h==16: 1 byte  start:4 | (len-1):4
 *                      h>16:  2 bytes start, len   (len==0 -> blank)
 *              body    PackBits stream producing exactly len rows; rows
 *                      outside [start, start+len) are zero.
 *              control c < 0x80: literal, c+1 symbols follow
 *                      c >= 0x80: repeat next symbol (c & 0x7F) + 2 times
 *              symbol  rb==1: raw row byte
 *              rb==2: u8 index into palette[]; 0xFF = escape,
 *                             2 raw row bytes (LE) follow inline
 *   palette  — rb==2 sizes only, <= 255 uint16_t rows, shared by the
 *              regular and bold faces of that size.
 */
typedef struct {
    uint16_t first_char;
    uint16_t last_char;
    const uint16_t *idx;   /* points into flash (or DRAM after font_init) */
} FontRange;

/* One face's complete table. The generated terminusWxH.c files export two
 * of these per size; everything else in them is static. */
typedef struct {
    const FontRange *ranges;
    const uint8_t   *pool;
    const uint16_t  *palette;    /* rb==2 sizes, shared regular/bold; else NULL */
    uint16_t         num_ranges;
    uint16_t         pool_bytes;
    uint16_t         palette_len;
    uint8_t          smear_left; /* bold faces: 1 = smear left (8x16 only) */
} FontFace;

#if FONT_RT_8X16
extern const FontFace terminus8x16_regular;
#if FONT_BOLD_ENABLED
extern const FontFace terminus8x16_bold;
#endif
#endif

#if FONT_RT_10X20
extern const FontFace terminus10x20_regular;
#if FONT_BOLD_ENABLED
extern const FontFace terminus10x20_bold;
#endif
#endif

#if FONT_RT_12X24
extern const FontFace terminus12x24_regular;
#if FONT_BOLD_ENABLED
extern const FontFace terminus12x24_bold;
#endif
#endif
