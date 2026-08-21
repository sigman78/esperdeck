/*
 * ISR render bench stress mode (CONFIG_CYBERDECK_BENCH_STRESS, default n).
 *
 * A development tool, not a user feature: instead of the app shell, boot
 * into a task that continuously repaints the whole grid with worst-case
 * dense colored text (25% bold) through vterm and logs the
 * CONFIG_DISPLAY_ISR_BENCH per-chunk cycle counters every 5 s. Used to
 * quantify render/decoder changes — see "Glyph tables & the row cache" in
 * docs/ARCHITECTURE.md for the numbers this produced.
 *
 * The task cycles through thirteen phases, one measured window each, tagged
 * `ph=` in the log line, sweeping three independent axes:
 *
 *   overlay  off / clear / scrim / spaces / dense / bars / bold — the
 *            shell's chrome layer, composited over a dense terminal
 *   content  t:sparse / t:blank — blank cells skip the glyph decode, so ISR
 *            cost swings ~29% with how much ink is on screen. Quoting a
 *            render-ISR number without its content mode is meaningless.
 *   effects  fx:app / fx:app+sp / fx:noscan / fx:none — a bench build
 *            returns before cyberdeck_app_init(), so display_fx_set() is
 *            never called and the wobble LUT stays flat. WOBBLE was
 *            therefore absent from every measurement taken before these
 *            phases existed, despite shipping enabled.
 *
 * Each phase names its own fx config so none inherits another's. See the
 * phase table in bench_stress.c for what each one isolates.
 *
 * Compiled out entirely when the option is off; the hooks below reduce to
 * no-ops.
 */
#pragma once
#include <stdbool.h>
#include "font.h"

#ifdef CONFIG_CYBERDECK_BENCH_STRESS

/** The size the bench was configured for (overrides the stored setting). */
font_size_t bench_stress_font_override(font_size_t normal);

/** Spawn the repaint task. Returns true: the bench owns the screen and the
 *  caller should skip the shell (and everything else) entirely. */
bool bench_stress_start(void);

#else

static inline font_size_t bench_stress_font_override(font_size_t normal)
{
    return normal;
}

static inline bool bench_stress_start(void) { return false; }

#endif
