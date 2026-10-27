/* Host substitute for the assembly boundary only. Tests execute the real
 * production dispatcher, margin fill and rejection code. */
#include "render_simd.h"
#include <string.h>
static void expand(const render_simd_args_t *a, unsigned w)
{
    unsigned band = w == 8 ? 16 : w, rb = w == 8 ? 1 : 2;
    for (unsigned c = 0; c < a->cols; c++)
    for (unsigned y = 0; y < band; y++) {
        unsigned bits = rb == 1 ? a->rows[c * 16 + y] :
            ((const uint16_t *)(a->rows + c * w * 4))[y];
        const uint32_t *p = (y & 1 ? a->odd : a->even)[c];
        uint16_t bg = (uint16_t)p[0], xf = (uint16_t)(p[3] ^ p[0]);
        uint16_t *out = (uint16_t *)((uint8_t *)a->dst + y * a->stride) + c * w;
        for (unsigned x = 0; x < 8; x++) out[x] = bg ^ (xf & a->masks[(bits >> (w - 8)) & 255][x]);
        for (unsigned x = 8; x < w; x += 2) {
            uint32_t pair = p[(bits >> (w - 2 - x)) & 3];
            memcpy(out + x, &pair, 4);
        }
    }
}
void render_simd_expand8(const render_simd_args_t *a) { expand(a, 8); }
void render_simd_expand10(const render_simd_args_t *a) { expand(a, 10); }
void render_simd_expand12(const render_simd_args_t *a) { expand(a, 12); }
static void shift(uint16_t *line, unsigned vectors, int offset)
{
    unsigned pixels = vectors * 8, n = (unsigned)(offset < 0 ? -offset : offset) * 2;
    if (offset > 0) {
        memmove(line + n, line, (pixels - n) * 2); memset(line, 0, n * 2);
    } else {
        memmove(line, line + n, (pixels - n) * 2); memset(line + pixels - n, 0, n * 2);
    }
}
#define SHIFT(n, off) void render_simd_shift_##n(uint16_t *p, unsigned v) { shift(p, v, off); }
SHIFT(m5,-5) SHIFT(m4,-4) SHIFT(m3,-3) SHIFT(m2,-2) SHIFT(m1,-1)
SHIFT(p1,1) SHIFT(p2,2) SHIFT(p3,3) SHIFT(p4,4)
