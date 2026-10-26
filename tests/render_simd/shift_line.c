#include "scan_variants.h"
#include "esp_attr.h"
void shift_m5(uint16_t *); void shift_m4(uint16_t *); void shift_m3(uint16_t *);
void shift_m2(uint16_t *); void shift_m1(uint16_t *);
void shift_p1(uint16_t *); void shift_p2(uint16_t *); void shift_p3(uint16_t *);
void shift_p4(uint16_t *);
static DRAM_ATTR void (*const shifts[])(uint16_t *) = {
    shift_m5, shift_m4, shift_m3, shift_m2, shift_m1, 0,
    shift_p1, shift_p2, shift_p3, shift_p4
};
int IRAM_ATTR shift_line_guarded(uint16_t *line, int offset)
{
    if (offset < -5 || offset > 4) return -1;
    if (offset) shifts[offset + 5](line);
    return 0;
}
