#include "scan_variants.h"
#include <string.h>

void scan_wide_model(uint16_t *dst, const uint8_t *rows,
                     const uint32_t (*even)[4], const uint32_t (*odd)[4],
                     const uint16_t (*masks)[8], unsigned row0, unsigned width)
{
    unsigned band = width == 8 ? 16 : width;
    unsigned stride = width == 8 ? 16 : width * 4;
    for (unsigned c = 0; c < 800 / width; c++) {
        for (unsigned y = 0; y < band; y++) {
            const uint32_t *pr = y & 1 ? odd[c] : even[c];
            uint16_t bg = (uint16_t)pr[0], xf = bg ^ (uint16_t)pr[3];
            unsigned bits = width == 8 ? rows[c * stride + row0 + y]
                : ((const uint16_t *)(rows + c * stride))[row0 + y];
            const uint16_t *mask = masks[(bits >> (width - 8)) & 255];
            uint16_t *out = dst + y * 800 + c * width;
            for (unsigned p = 0; p < 8; p++) out[p] = bg ^ (xf & mask[p]);
            for (unsigned p = 8; p < width; p += 2) {
                uint32_t pair = pr[(bits >> (width - p - 2)) & 3];
                out[p] = (uint16_t)pair;
                out[p + 1] = (uint16_t)(pair >> 16);
            }
        }
    }
}

void shift_line_model(uint16_t *line, int offset)
{
    uint16_t original[800];
    memcpy(original, line, sizeof original);
    /* Model concatenating adjacent aligned 8-pixel blocks, including zero
     * blocks beyond the line. The independent test uses glyph coordinates. */
    for (int block = 0; block < 100; block++) {
        int source = block * 8 - offset * 2;
        int base = source >= 0 ? source / 8 : (source - 7) / 8;
        int skip = source - base * 8;
        uint16_t joined[16] = {0};
        for (int k = 0; k < 16; k++) {
            int x = base * 8 + k;
            if (x >= 0 && x < 800) joined[k] = original[x];
        }
        memcpy(line + block * 8, joined + skip, 16);
    }
}
