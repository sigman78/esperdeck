/*
 * color — ANSI-256 + truecolor → RGB565 conversion
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

/* Default terminal colors (RGB565). */
#define COLOR_DEFAULT_FG  0xBDF7u   /* ANSI 7 — light gray */
#define COLOR_DEFAULT_BG  0x0000u   /* black */

/* Convert RGB components (0–255 each) to RGB565. */
static inline uint16_t color_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* Convert an ANSI-256 palette index to RGB565.
 * Covers: 0–15 named, 16–231 6×6×6 cube, 232–255 grayscale. */
uint16_t color_ansi(uint8_t idx);
