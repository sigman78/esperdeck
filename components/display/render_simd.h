#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Internal two-pass scan contract. Width is the configured panel width,
 * not the glyph grid width. A false return leaves all output untouched. */
typedef struct {
    uint16_t *dst;
    const uint8_t *rows;
    const uint32_t (*pairs[2])[4];
    const int8_t *offsets;
    unsigned width, font_width, cols, num_scans, row0;
} render_simd_ctx_t;
bool render_simd_scan(const render_simd_ctx_t *cx);

/* Assembly ABI: pointer fields occupy four bytes on the S3. rows already
 * points at row0. Full 16/10/12-line bands; line stride is in bytes. */
typedef struct {
    uint16_t *dst;
    const uint8_t *rows;
    const uint32_t (*even)[4], (*odd)[4];
    const uint16_t (*masks)[8];
    unsigned cols, stride;
} render_simd_args_t;
void render_simd_expand8(const render_simd_args_t *);
void render_simd_expand10(const render_simd_args_t *);
void render_simd_expand12(const render_simd_args_t *);
