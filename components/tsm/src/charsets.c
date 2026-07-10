/*
 * charsets.c — VT character set translation tables
 *
 * SPDX-License-Identifier: MIT
 */
#include "charsets.h"

/* ── DEC Special Graphics (VT100 "Line Drawing") ──────────────────────────
 *
 * ESC ( 0  (or ESC ) 0) selects this charset for G0 (or G1).
 * Only bytes 0x60–0x7E are remapped; everything else passes through.
 * Table indexed by (byte - 0x60), 31 entries.
 */
static const uint16_t s_dec_gfx[31] = {
    0x25C6,  /* 0x60 '`'  ◆ diamond                  */
    0x2592,  /* 0x61 'a'  ▒ medium shade              */
    0x2409,  /* 0x62 'b'  HT symbol                   */
    0x240C,  /* 0x63 'c'  FF symbol                   */
    0x240D,  /* 0x64 'd'  CR symbol                   */
    0x240A,  /* 0x65 'e'  LF symbol                   */
    0x00B0,  /* 0x66 'f'  ° degree                    */
    0x00B1,  /* 0x67 'g'  ± plus-minus                */
    0x2424,  /* 0x68 'h'  NL symbol                   */
    0x240B,  /* 0x69 'i'  VT symbol                   */
    0x2518,  /* 0x6A 'j'  ┘ box lower-right           */
    0x2510,  /* 0x6B 'k'  ┐ box upper-right           */
    0x250C,  /* 0x6C 'l'  ┌ box upper-left            */
    0x2514,  /* 0x6D 'm'  └ box lower-left            */
    0x253C,  /* 0x6E 'n'  ┼ box cross                 */
    0x23BA,  /* 0x6F 'o'  ⎺ scan-line 1               */
    0x23BB,  /* 0x70 'p'  ⎻ scan-line 3               */
    0x2500,  /* 0x71 'q'  ─ horizontal line           */
    0x23BC,  /* 0x72 'r'  ⎼ scan-line 7               */
    0x23BD,  /* 0x73 's'  ⎽ scan-line 9               */
    0x251C,  /* 0x74 't'  ├ box left-tee              */
    0x2524,  /* 0x75 'u'  ┤ box right-tee             */
    0x2534,  /* 0x76 'v'  ┴ box bottom-tee            */
    0x252C,  /* 0x77 'w'  ┬ box top-tee               */
    0x2502,  /* 0x78 'x'  │ vertical line             */
    0x2264,  /* 0x79 'y'  ≤ less-or-equal             */
    0x2265,  /* 0x7A 'z'  ≥ greater-or-equal          */
    0x03C0,  /* 0x7B '{'  π pi                        */
    0x2260,  /* 0x7C '|'  ≠ not-equal                 */
    0x00A3,  /* 0x7D '}'  £ pound                     */
    0x00B7,  /* 0x7E '~'  · middle dot                */
};

/* ── Public API ──────────────────────────────────────────────────────────── */

uint16_t charset_xlat(charset_id_t cs, uint8_t cp)
{
    if (cs == CHARSET_DEC_GFX && cp >= 0x60u && cp <= 0x7Eu) {
        return s_dec_gfx[cp - 0x60u];
    }
    return (uint16_t)cp;
}
