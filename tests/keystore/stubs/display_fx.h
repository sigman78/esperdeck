/* Test stub — storage.c only needs the display_fx_cfg_t field layout for
 * its fx.ini load/save table (offsetof). Values are never interpreted. */
#pragma once
#include <stdint.h>

typedef struct {
    uint8_t scanlines;
    uint8_t bold_pop;
    uint8_t mono;
    uint8_t glow;
    uint8_t glow_frames;
    uint8_t glow_strength;
    uint8_t wipe;
    uint8_t wipe_frames;
    uint8_t collapse;
    uint8_t collapse_frames;
    uint8_t static_burst;
    uint8_t static_frames;
    uint8_t static_lines;
    uint8_t wobble;
} display_fx_cfg_t;
