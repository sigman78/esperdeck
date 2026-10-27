#include "render_simd.h"
#include "esp_attr.h"
#include <stddef.h>

#ifdef ESP_PLATFORM
_Static_assert(offsetof(render_simd_args_t, stride) == 24, "SIMD assembly ABI");
#endif

#define BIT_MASK(n, b) ((n) & (1u << (b)) ? 0xffff : 0)
#define MASK_ROW(n) {BIT_MASK(n,7), BIT_MASK(n,6), BIT_MASK(n,5), BIT_MASK(n,4), \
                     BIT_MASK(n,3), BIT_MASK(n,2), BIT_MASK(n,1), BIT_MASK(n,0)}
static _Alignas(16) DRAM_ATTR const uint16_t s_simd_masks[256][8] = {
#include "render_simd_masks.inc"
};
#undef MASK_ROW
#undef BIT_MASK

typedef void (*shift_fn)(uint16_t *, unsigned);
#define SHIFT_DECL(n) extern void render_simd_shift_##n(uint16_t *, unsigned)
SHIFT_DECL(m5); SHIFT_DECL(m4); SHIFT_DECL(m3); SHIFT_DECL(m2); SHIFT_DECL(m1);
SHIFT_DECL(p1); SHIFT_DECL(p2); SHIFT_DECL(p3); SHIFT_DECL(p4);
static DRAM_ATTR const shift_fn s_simd_shifts[] = {
    render_simd_shift_m5, render_simd_shift_m4, render_simd_shift_m3,
    render_simd_shift_m2, render_simd_shift_m1, NULL,
    render_simd_shift_p1, render_simd_shift_p2, render_simd_shift_p3, render_simd_shift_p4
};

bool IRAM_ATTR render_simd_scan(const render_simd_ctx_t *cx)
{
    unsigned fw = cx->font_width;
    unsigned band = fw == 8 ? 16 : fw;
    if ((fw != 8 && fw != 10 && fw != 12) || cx->width < 16 ||
        (cx->width & 7) || cx->width > UINT32_MAX / 32 ||
        ((uintptr_t)cx->dst & 3) || cx->cols > cx->width / fw ||
        cx->num_scans != band || cx->row0 % band || cx->row0 >= fw * 2)
        return false;
    if (cx->offsets)
        for (unsigned n = 0; n < band; n++)
            if (cx->offsets[n] < -5 || cx->offsets[n] > 4 ||
                (cx->offsets[n] && ((uintptr_t)cx->dst & 15))) return false;

    const render_simd_args_t args = {
        cx->dst, cx->rows + cx->row0 * (fw == 8 ? 1 : 2),
        cx->pairs[0], cx->pairs[1], s_simd_masks, cx->cols, cx->width * 2
    };
    if (cx->cols) {
        if (fw == 8) render_simd_expand8(&args);
        else if (fw == 10) render_simd_expand10(&args);
        else render_simd_expand12(&args);
    }
    unsigned used = cx->cols * fw / 2, words = cx->width / 2;
    if (used != words)
        for (unsigned n = 0; n < band; n++) {
            uint32_t *line = (uint32_t *)(cx->dst + n * cx->width);
            for (unsigned i = used; i < words; i++) line[i] = 0;
        }
    if (cx->offsets)
        for (unsigned n = 0; n < band; n++) {
            int off = cx->offsets[n];
            if (!off) continue;
            uint16_t *line = cx->dst + n * cx->width;
            s_simd_shifts[off + 5](line, cx->width / 8);
        }
    return true;
}
