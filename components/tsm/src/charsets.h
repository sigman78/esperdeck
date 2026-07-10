/*
 * charsets — VT character set translation
 *
 * Supported designations (ESC ( <id> / ESC ) <id>):
 *   'B'  US ASCII            — identity mapping (default)
 *   '0'  DEC Special Graphics — box-drawing + misc symbols
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

typedef enum {
    CHARSET_ASCII = 0,   /* G0/G1 = US ASCII (identity) */
    CHARSET_DEC_GFX,     /* G0/G1 = DEC Special Graphics ('0') */
} charset_id_t;

/* Translate a 7-bit codepoint through the given charset.
 * Returns the Unicode codepoint to emit (identity for ASCII). */
uint16_t charset_xlat(charset_id_t cs, uint8_t cp);
