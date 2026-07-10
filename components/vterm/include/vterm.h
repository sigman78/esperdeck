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
