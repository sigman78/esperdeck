#include "scan8_fused.h"
#include "esp_attr.h"
typedef void (*line_fn)(uint16_t *, const uint8_t *, const uint32_t (*)[4],
                        const uint16_t (*)[8], int);
#define DECL(name) extern void name(uint16_t *, const uint8_t *, const uint32_t (*)[4], const uint16_t (*)[8], int)
DECL(fused_m5); DECL(fused_m4); DECL(fused_m3); DECL(fused_m2); DECL(fused_m1);
DECL(fused_0); DECL(fused_p1); DECL(fused_p2); DECL(fused_p3); DECL(fused_p4);
static DRAM_ATTR const line_fn variants[] = {
    fused_m5, fused_m4, fused_m3, fused_m2, fused_m1,
    fused_0, fused_p1, fused_p2, fused_p3, fused_p4
};
void IRAM_ATTR scan8_fused_line(uint16_t *dst, const uint8_t *rows,
                               const uint32_t (*pairs)[4],
                               const uint16_t (*masks)[8], int off)
{
    variants[off + 5](dst, rows, pairs, masks, off);
}
