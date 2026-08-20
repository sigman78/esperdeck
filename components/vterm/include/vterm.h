/*
 * vterm -- VT/ANSI terminal emulator.
 *
 * Parses VT100...xterm byte streams and drives the display cell buffer
 * directly.  Use vterm_write() to feed raw SSH/PTY data.
 */

#ifndef VTERM_H
#define VTERM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/**
 * Initialize the VT emulator.
 *
 * @param cols  Terminal width in character columns.
 * @param rows  Terminal height in character rows.
 */
esp_err_t vterm_init(int cols, int rows);

/**
 * Feed raw bytes from the remote (SSH/PTY) into the VT parser.
 * The display cell buffer is refreshed on return (unless a ?2026
 * synchronized-output update is in progress).
 *
 * @param data  Byte stream (may contain VT escape sequences).
 * @param len   Number of bytes.
 */
void vterm_write(const char *data, size_t len);

/**
 * Feed raw bytes WITHOUT refreshing the display cell buffer.
 * Used by the SSH drain loop to parse a batch of chunks and present
 * once via vterm_flush() — see docs/speedup-render.md.
 */
void vterm_feed(const char *data, size_t len);

/**
 * Copy dirty rows to the display cell buffer and update the cursor.
 * No-op while a ?2026 synchronized-output update is in progress
 * (the frame is presented when the remote sends ESU).
 */
void vterm_flush(void);

/*
 * Re-assert the hardware cursor from current terminal state. Full-screen
 * overlays park the cursor (ui_no_cursor) and a flush only happens when the
 * host sends bytes — call this when handing the screen back to the session
 * so the cursor returns without waiting for input.
 */
void vterm_cursor_refresh(void);

/**
 * Register a callback for bytes the terminal state machine needs to
 * send back to the remote (cursor-position reports, DA1 responses, ...).
 * Pass NULL to disable.
 *
 * @param cb    Callback function pointer.
 * @param user  Opaque pointer forwarded to the callback.
 */
typedef void (*vterm_response_cb_t)(const char *data, size_t len, void *user);
void vterm_set_response_cb(vterm_response_cb_t cb, void *user);

/**
 * Reset the VT state machine to its initial state.
 */
void vterm_reset(void);

/*
 * Scrollback — history that has scrolled off the top of the screen.
 *
 * Fed only by the primary screen; alt-screen apps (vim, htop) never
 * contribute and cannot be scrolled. Capacity is
 * CONFIG_VTERM_SCROLLBACK_LINES, 0 disables the whole feature and these
 * calls become no-ops reporting 0.
 *
 * The screen is repainted as needed by these calls, so a caller does not
 * follow them with vterm_flush().
 */

/** Move the view @p delta rows (positive = back in time). Returns the new
 *  offset, clamped to the stored history. */
int vterm_scroll(int delta);

/** Half-screen step: @p dir +1 = back in time, -1 = towards live. */
int vterm_scroll_page(int dir);

/** Return to the live view. Returns true if the view actually moved —
 *  callers use this to swallow the keystroke that caused it. */
bool vterm_scroll_reset(void);

/** Current view offset in rows; 0 = live. */
int vterm_scroll_offset(void);

/** Rows of history currently available to scroll back through. */
int vterm_scroll_len(void);

/** Scrollback capacity, 0 when the feature is off. Use this — not
 *  vterm_scroll_len(), which is also 0 on a fresh session — to decide
 *  whether the deck should claim the scrollback key bindings at all. */
int vterm_scroll_capacity(void);

/**
 * Returns true when the remote has enabled application cursor key mode
 * (DECCKM, ESC [ ? 1 h).  Use this to decide whether arrow keys should
 * be sent as ESC O A/B/C/D (application) or ESC [ A/B/C/D (normal).
 */
bool vterm_app_cursor_keys(void);

/**
 * Log a performance summary (flushes, bytes, tsm cycles, draw cycles).
 * No-op when CONFIG_VTERM_BENCH is disabled.
 */
void vterm_bench_report(void);

/**
 * Clear all performance accumulators.
 * No-op when CONFIG_VTERM_BENCH is disabled.
 */
void vterm_bench_reset(void);

#endif /* VTERM_H */
