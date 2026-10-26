#include "scan8_fused.h"

static void expand(uint16_t *v, int c, const uint8_t *rows,
                   const uint32_t (*pairs)[4], const uint16_t (*masks)[8])
{
    for (unsigned p = 0; p < 8; p++) {
        v[p] = 0;
        if (c >= 0 && c < 100) {
            uint16_t bg = (uint16_t)pairs[c][0];
            uint16_t xf = (uint16_t)pairs[c][3] ^ bg;
            v[p] = bg ^ (xf & masks[rows[c * 16]][p]);
        }
    }
}

void scan8_fused_line_model(uint16_t *dst, const uint8_t *rows,
                            const uint32_t (*pairs)[4],
                            const uint16_t (*masks)[8], int off)
{
    int source = -off * 2;
    int first = source >= 0 ? source / 8 : (source - 7) / 8;
    unsigned rem = (unsigned)(source - first * 8);
    uint16_t previous[8], current[8];
    expand(previous, first, rows, pairs, masks);
    for (unsigned v = 0; v < 100; v++) {
        expand(current, first + (int)v + 1, rows, pairs, masks);
        for (unsigned p = 0; p < 8; p++)
            dst[v * 8 + p] = p + rem < 8 ? previous[p + rem] : current[p + rem - 8];
        for (unsigned p = 0; p < 8; p++) previous[p] = current[p];
    }
}
