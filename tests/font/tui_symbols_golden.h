/* CRC32 references for the reviewed original symbol drawings. */
#pragma once

typedef struct { uint16_t cp; uint32_t crc; } symbol_golden_t;

#if FONT_RT_8X16
static const symbol_golden_t symbol_golden[] = {
    {0x2298, 0xCF75E068u},
    {0x2299, 0x7606204Au},
    {0x231B, 0x7BB4C828u},
    {0x23F1, 0xC4350205u},
    {0x23F2, 0x20EB80F1u},
    {0x25D0, 0x080C9828u},
    {0x26A0, 0xF66D69FBu},
    {0x276F, 0x15A4AC83u},
};
#elif FONT_RT_10X20
static const symbol_golden_t symbol_golden[] = {
    {0x2298, 0x733B4E7Du},
    {0x2299, 0x058B406Fu},
    {0x231B, 0x8852E417u},
    {0x23F1, 0x0BE03F3Cu},
    {0x23F2, 0xEF72976Cu},
    {0x25D0, 0xE068E862u},
    {0x26A0, 0xFD062DA1u},
    {0x276F, 0x62D21808u},
};
#elif FONT_RT_12X24
static const symbol_golden_t symbol_golden[] = {
    {0x2298, 0x45C0BD2Eu},
    {0x2299, 0x71B92AD3u},
    {0x231B, 0xD2734DB7u},
    {0x23F1, 0xD9AF3097u},
    {0x23F2, 0x08DEC582u},
    {0x25D0, 0xC7E00A93u},
    {0x26A0, 0x78971663u},
    {0x276F, 0xDDA20DA1u},
};
#endif
