/*
 * display_fx.c — owner of the runtime render-effect state.
 *
 * The rendering itself happens in display_render.c; this file only holds
 * the DRAM-resident config/state and the task-side API (set/get, event
 * arming, row stamps). Compiled for both the ESP32 target and the
 * PC simulator.
 */

#include "display_fx_internal.h"
#include <math.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Shared state (see display_fx_internal.h)
 * ---------------------------------------------------------------------- */
DRAM_ATTR display_fx_cfg_t g_fx_cfg = {
    .scanlines       = 1,
    .bold_pop        = 1,
    .mono            = 0,
    .glow            = 0,    /* judged a failure on-glass — off by default */
    .glow_frames     = 20,   /* ~0.5 s */
    .glow_strength   = 0,
    .wipe            = 1,
    .wipe_frames     = 18,   /* ~0.46 s */
    .collapse        = 1,
    .collapse_frames = 12,   /* ~0.3 s */
    .static_burst    = 1,
    .static_frames   = 10,   /* ~0.25 s */
    .static_lines    = 2,
    .wobble          = 2,    /* medium ±4 px — prototype default, on trial */
};

DRAM_ATTR volatile uint8_t g_fx_frame = 0;

/* Wobble LUT starts flat (matches .wobble = 0); display_fx_set rebuilds it
 * whenever a config is applied. */
DRAM_ATTR int8_t g_fx_wobble_lut[256] = { 0 };

#if DISPLAY_FX_ROW_GLOW
/* Glow accent: warm phosphor amber. Blend terms are per-field shift/mask
 * fractions of this color (carry-safe RGB565 math). */
#define FX_GLOW_ACCENT  ((uint16_t)(((255 & 0xF8) << 8) | ((144 & 0xFC) << 3) | (32 >> 3)))

DRAM_ATTR uint16_t g_fx_glow_acc[2] = {
    (uint16_t)((FX_GLOW_ACCENT >> 3) & 0x18E3),   /* 12.5% */
    (uint16_t)((FX_GLOW_ACCENT >> 2) & 0x39E7),   /* 25%   */
};
DRAM_ATTR volatile uint8_t g_fx_row_stamp[DISPLAY_FX_MAX_ROWS] = {
    /* start "old" so nothing glows at power-on */
    [0 ... DISPLAY_FX_MAX_ROWS - 1] = 0x80,
};
#endif

DRAM_ATTR volatile int16_t g_fx_wipe_left      = 0;
DRAM_ATTR volatile int16_t g_fx_wipe_total     = 0;
DRAM_ATTR volatile int16_t g_fx_collapse_left  = 0;
DRAM_ATTR volatile int16_t g_fx_collapse_total = 0;
DRAM_ATTR volatile int16_t g_fx_static_left    = 0;

static uint8_t clamp_u8(uint8_t v, uint8_t lo, uint8_t hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

void display_fx_defaults(display_fx_cfg_t *out)
{
    static const display_fx_cfg_t def = {
        .scanlines = 1,
        .bold_pop = 1,   .mono = 0,
        .glow = 0,       .glow_frames = 20,     .glow_strength = 0,
        .wipe = 1,       .wipe_frames = 18,
        .collapse = 1,   .collapse_frames = 12,
        .static_burst = 1, .static_frames = 10, .static_lines = 2,
        .wobble = 2,   /* medium ±4 px — prototype default, on trial */
    };
    if (out) *out = def;
}

void display_fx_get(display_fx_cfg_t *out)
{
    if (out) *out = g_fx_cfg;
}

void display_fx_set(const display_fx_cfg_t *cfg)
{
    if (!cfg) return;
    display_fx_cfg_t c = *cfg;

    c.scanlines       = !!c.scanlines;
    c.bold_pop        = !!c.bold_pop;
    c.mono            = clamp_u8(c.mono, 0, 2);
    c.glow            = !!c.glow;
    c.glow_frames     = clamp_u8(c.glow_frames, 1, 120);
    c.glow_strength   = clamp_u8(c.glow_strength, 0, 1);
    c.wipe            = !!c.wipe;
    c.wipe_frames     = clamp_u8(c.wipe_frames, 4, 120);
    c.collapse        = !!c.collapse;
    c.collapse_frames = clamp_u8(c.collapse_frames, 4, 120);
    c.static_burst    = !!c.static_burst;
    c.static_frames   = clamp_u8(c.static_frames, 1, 120);
    c.static_lines    = clamp_u8(c.static_lines, 1, 4);
    c.wobble          = clamp_u8(c.wobble, 0, 3);

    /* Rebuild the wobble LUT for the active amplitude (task context — the
     * ISR reads single bytes, so a mid-rebuild frame shows at worst a
     * one-frame ripple). Amplitude = 2 px per level. */
    for (int i = 0; i < 256; i++)
        g_fx_wobble_lut[i] = (int8_t)lrintf(
            2.0f * (float)c.wobble * sinf((float)i * (6.2831853f / 256.0f)));

    g_fx_cfg = c;   /* byte fields; a torn update is a one-frame glitch */
}

void display_fx_wipe(void)
{
    if (!g_fx_cfg.wipe) return;
    g_fx_collapse_left = 0;                       /* mutually exclusive */
    /* Disarm before rewriting total: the ISR must never pair a new total
     * with a stale, larger counter (negative window for one frame). */
    g_fx_wipe_left     = 0;
    g_fx_wipe_total    = g_fx_cfg.wipe_frames;
    g_fx_wipe_left     = g_fx_cfg.wipe_frames;
}

void display_fx_collapse(void)
{
    if (!g_fx_cfg.collapse) return;
    g_fx_wipe_left      = 0;                      /* mutually exclusive */
    g_fx_collapse_left  = 0;                      /* disarm before rewrite */
    g_fx_collapse_total = g_fx_cfg.collapse_frames;
    g_fx_collapse_left  = g_fx_cfg.collapse_frames;
}

void display_fx_static(void)
{
    if (!g_fx_cfg.static_burst) return;
    g_fx_static_left = g_fx_cfg.static_frames;
}

void display_fx_static_brief(void)
{
    if (!g_fx_cfg.static_burst) return;
    int16_t n = (int16_t)(g_fx_cfg.static_frames / 2);
    if (n < 1) n = 1;
    if (g_fx_static_left < n) g_fx_static_left = n;   /* extend, never cut */
}

void display_fx_touch_row(int row)
{
#if DISPLAY_FX_ROW_GLOW
    if ((unsigned)row < DISPLAY_FX_MAX_ROWS)
        g_fx_row_stamp[row] = g_fx_frame;
#else
    (void)row;
#endif
}
