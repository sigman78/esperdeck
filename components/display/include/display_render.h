/*
 * display_render.h — internal header for the shared rendering core.
 *
 * Included by lcd_driver.c (ESP32 ISR wrapper) and display_sdl.c
 * (SDL2 simulator backend).  NOT part of the public display API.
 */

#ifndef DISPLAY_RENDER_H
#define DISPLAY_RENDER_H

#include "display.h"

/**
 * Render one horizontal band of the terminal into a pixel buffer.
 *
 * The ESP32 bounce-buffer ISR and the SDL2 frame loop share this one
 * glyph→pixel implementation.
 *
 * @param dst      Destination RGB565 pixel buffer (32-bit aligned).
 * @param pos_px   Index of the first pixel in the full framebuffer
 *                 (= start_scanline * DISPLAY_WIDTH).
 * @param n_bytes  Byte count of the band
 *                 (= DISPLAY_WIDTH × BOUNCE_BUFFER_HEIGHT × sizeof(color_t)).
 */
void display_render_chunk(color_t *dst, int pos_px, int n_bytes);

#endif /* DISPLAY_RENDER_H */
