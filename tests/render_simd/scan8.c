#include "scan8.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define IRAM_ATTR
#endif

/* Same pair-table emission and paired columns as render_scan.inc, without
 * dispatch, wobble, effects or margin clearing. Both kernels leave margins. */
IRAM_ATTR void scan8_lut(uint16_t *dst, const uint8_t *rows,
                         const uint32_t (*pairs)[4],
                         const uint16_t masks[256][8], unsigned ncols)
{
    (void)masks;
    for (unsigned r = 0; r < 16; r++) {
        uint32_t *d = (uint32_t *)(dst + r * 800);
        unsigned c = 0;
        for (; c + 1 < ncols; c += 2) {
            unsigned b0 = rows[c * 16 + r], b1 = rows[(c + 1) * 16 + r];
            const uint32_t *t0 = pairs[c], *t1 = pairs[c + 1];
            #pragma GCC unroll 8
            for (unsigned p = 0; p < 8; p += 2)
                d[p / 2] = t0[(b0 >> (6 - p)) & 3];
            #pragma GCC unroll 8
            for (unsigned p = 0; p < 8; p += 2)
                d[4 + p / 2] = t1[(b1 >> (6 - p)) & 3];
            d += 8;
        }
        if (c < ncols) {
            unsigned b = rows[c * 16 + r];
            #pragma GCC unroll 8
            for (unsigned p = 0; p < 8; p += 2)
                d[p / 2] = pairs[c][(b >> (6 - p)) & 3];
        }
    }
}

void scan8_masks_init(uint16_t masks[256][8])
{
    for (unsigned b = 0; b < 256; b++)
        for (unsigned p = 0; p < 8; p++)
            masks[b][p] = (b & (0x80u >> p)) ? UINT16_MAX : 0;
}

void scan8_model(uint16_t *dst, const uint8_t *rows,
                 const uint32_t (*pairs)[4], const uint16_t masks[256][8],
                 unsigned ncols)
{
    for (unsigned c = 0; c < ncols; c++) {
        const uint16_t bg = (uint16_t)pairs[c][0];
        const uint16_t xf = bg ^ (uint16_t)pairs[c][3];
        for (unsigned r = 0; r < 16; r++) {
            const uint16_t *m = masks[rows[c * 16 + r]];
            for (unsigned p = 0; p < 8; p++)
                dst[r * 800 + c * 8 + p] = bg ^ (xf & m[p]);
        }
    }
}
