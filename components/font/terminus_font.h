#pragma once
#include <stdint.h>
#include <stddef.h>
#include "font.h"

/*
 * Compressed glyph table format ("v1") — see tools/gen_terminus.py.
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
 *                      rb==2: u8 index into palette[]; 0xFF = escape,
 *                             2 raw row bytes (LE) follow inline
 *   palette  — rb==2 sizes only, <= 255 uint16_t rows, shared by the
 *              regular and bold faces of that size.
 */
typedef struct {
    uint16_t first_char;
    uint16_t last_char;
    const uint16_t *idx;   /* points into flash (or DRAM after font_init) */
} FontRange;

#if FONT_RT_8X16
extern const FontRange terminus8x16_ranges[];
extern const int       terminus8x16_num_ranges;
extern const uint8_t   terminus8x16_pool[];
extern const uint16_t  terminus8x16_pool_bytes;
#if FONT_BOLD_ENABLED
extern const FontRange terminus8x16_bold_ranges[];
extern const int       terminus8x16_num_bold_ranges;
extern const uint8_t   terminus8x16_bold_pool[];
extern const uint16_t  terminus8x16_bold_pool_bytes;
extern const uint8_t   terminus8x16_bold_smear_left;
#endif
#endif

#if FONT_RT_10X20
extern const FontRange terminus10x20_ranges[];
extern const int       terminus10x20_num_ranges;
extern const uint8_t   terminus10x20_pool[];
extern const uint16_t  terminus10x20_pool_bytes;
extern const uint16_t  terminus10x20_palette[];
extern const uint16_t  terminus10x20_palette_len;
#if FONT_BOLD_ENABLED
extern const FontRange terminus10x20_bold_ranges[];
extern const int       terminus10x20_num_bold_ranges;
extern const uint8_t   terminus10x20_bold_pool[];
extern const uint16_t  terminus10x20_bold_pool_bytes;
extern const uint8_t   terminus10x20_bold_smear_left;
#endif
#endif

#if FONT_RT_12X24
extern const FontRange terminus12x24_ranges[];
extern const int       terminus12x24_num_ranges;
extern const uint8_t   terminus12x24_pool[];
extern const uint16_t  terminus12x24_pool_bytes;
extern const uint16_t  terminus12x24_palette[];
extern const uint16_t  terminus12x24_palette_len;
#if FONT_BOLD_ENABLED
extern const FontRange terminus12x24_bold_ranges[];
extern const int       terminus12x24_num_bold_ranges;
extern const uint8_t   terminus12x24_bold_pool[];
extern const uint16_t  terminus12x24_bold_pool_bytes;
extern const uint8_t   terminus12x24_bold_smear_left;
#endif
#endif
