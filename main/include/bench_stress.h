/*
 * ISR render bench stress mode (CONFIG_CYBERDECK_BENCH_STRESS, default n).
 *
 * A development tool, not a user feature. Instead of the app shell, it
 * boots into a task that repaints the whole grid through vterm. That task
 * logs the CONFIG_DISPLAY_ISR_BENCH per-chunk cycle counters. It sweeps
 * phases along three axes: overlay, terminal content, and effect config.
 * What each phase isolates, and what invalidates a measurement, is
 * docs/bench-methodology.md; results live in docs/performance.md.
 *
 * Compiled out entirely when the option is off; the hooks below reduce to
 * no-ops.
 */
#pragma once
#include <stdbool.h>
#include "font.h"

#ifdef CONFIG_CYBERDECK_BENCH_STRESS

/** The size the build sets for the bench (overrides the stored setting). */
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
