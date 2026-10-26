#pragma once
#include <stdint.h>
/* One 800-pixel line, aligned destination and masks. rows points at the
 * selected scanline in 100 decoded 8x16 cells; pairs is its selected palette.
 * Offset is -5..4 32-bit words. */
void scan8_fused_line(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                      const uint16_t (*)[8], int);
void scan8_fused_line_model(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                            const uint16_t (*)[8], int);
int scan8_fused_test(void);
void scan8_fused_bench(uint16_t *);
