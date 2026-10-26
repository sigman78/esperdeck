#pragma once
#include <stdint.h>

void scan_wide_model(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                     const uint32_t (*)[4], const uint16_t (*)[8], unsigned, unsigned);
void shift_line_model(uint16_t *, int);
void scan10_guarded(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                    const uint32_t (*)[4], const uint16_t (*)[8], unsigned);
void scan12_guarded(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                    const uint32_t (*)[4], const uint16_t (*)[8], unsigned);
/* In-place, exactly 800 RGB565 pixels; aligned base, offset in 32-bit words.
 * Returns 0 for supported offsets -5..4, -1 otherwise without mutation. */
int shift_line_guarded(uint16_t *, int);
int scan_variants_test(void);
