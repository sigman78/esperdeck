/*
 * display_fx.h — runtime-tunable render effects for the shared render core.
 *
 * All effects execute inside display_render_chunk (the ESP32 bounce-buffer
 * ISR / the simulator frame loop). The design rule: per-cell and per-scanline
 * work only — nothing per-pixel over the full band except transient,
 * frame-bounded event effects.
 *
 * Config writes are plain byte stores read by the ISR; a torn update is at
 * worst a single-frame visual glitch, so no locking is needed.
 */

#ifndef DISPLAY_FX_H
#define DISPLAY_FX_H

#include <stdint.h>

/* Row-recency back glow is experimental: the idea fits the phosphor/mono
 * modes, but the current implementation didn't hold up on-glass (2026-07).
 * Compiled out by default — build with -DDISPLAY_FX_ROW_GLOW=1 to
 * experiment. The cfg fields and fx.ini keys survive either way so saved
 * settings round-trip across the gate. */
#ifndef DISPLAY_FX_ROW_GLOW
#define DISPLAY_FX_ROW_GLOW 0
#endif

typedef struct {
    /* CRT scanlines — every other scanline rendered at 93.75% brightness.
     * A single fixed level: anything stronger read as bars on-glass. */
    uint8_t scanlines;        /* 0 off / 1 on                              */

    /* Bold phosphor pop — ATTR_BOLD glyphs get ~1.5x brighter fg. */
    uint8_t bold_pop;         /* 0 off / 1 on                              */

    /* Monochrome phosphor mode — luma-map every color onto a CRT ramp. */
    uint8_t mono;             /* 0 off / 1 green / 2 amber                 */

    /* Row-recency back glow — rows with fresh output carry a warm bg
     * tint that cools off over glow_frames. */
    uint8_t glow;             /* 0 off / 1 on                              */
    uint8_t glow_frames;      /* duration in frames (~39/s), 1..120        */
    uint8_t glow_strength;    /* 0 = subtle (12.5%), 1 = strong (25%)      */

    /* Connect raster reveal — screen wipes in top-to-bottom behind a
     * cyan leading edge. Armed via display_fx_wipe(). */
    uint8_t wipe;             /* 0 off / 1 on                              */
    uint8_t wipe_frames;      /* sweep duration in frames, 4..120          */

    /* Disconnect CRT collapse — picture collapses to a bright center
     * line, then black. Armed via display_fx_collapse(). */
    uint8_t collapse;         /* 0 off / 1 on                              */
    uint8_t collapse_frames;  /* collapse duration in frames, 4..120       */

    /* Signal-loss static burst — noise snow on a few scanlines per band.
     * Armed via display_fx_static(). */
    uint8_t static_burst;     /* 0 off / 1 on                              */
    uint8_t static_frames;    /* burst duration in frames, 1..120          */
    uint8_t static_lines;     /* noisy scanlines per 16-line band, 1..4    */

    /* CRT line wobble — a ~16-scanline S-wiggle (a full vertical sine:
     * two opposite bulges) sweeps down the screen (~5 s visible pass,
     * ~1.6 s off-screen rest); bulge depth follows a temporal sine, one
     * cycle per sweep — the analog tracking-line instability look. */
    uint8_t wobble;           /* 0 off / 1 ±2 px / 2 ±4 px / 3 ±6 px       */
} display_fx_cfg_t;

/** Fill @p out with the default effect configuration. */
void display_fx_defaults(display_fx_cfg_t *out);

/** Copy the active configuration into @p out. */
void display_fx_get(display_fx_cfg_t *out);

/** Apply (and range-clamp) a new configuration. Safe from any task. */
void display_fx_set(const display_fx_cfg_t *cfg);

/* Event triggers. Each is a no-op when the effect is disabled in the
 * active config. Safe to call from any task. */
void display_fx_wipe(void);       /* raster reveal (connect / boot done)   */
void display_fx_collapse(void);   /* CRT power-off collapse (disconnect)   */
void display_fx_static(void);     /* signal-loss static burst              */
/** Half-length static burst (terminal BEL). Never shortens a burst already
 *  in flight — a bell during a disconnect burst leaves the long one alone. */
void display_fx_static_brief(void);

/** Mark a character row as freshly changed (drives the row back glow).
 *  Call from the vterm present path for each row whose cells were copied
 *  this flush. One byte store — safe from any task. No-op unless built
 *  with DISPLAY_FX_ROW_GLOW=1. */
void display_fx_touch_row(int row);

#endif /* DISPLAY_FX_H */
