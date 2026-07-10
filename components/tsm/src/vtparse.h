/*
 * vtparse — VT byte-stream parser
 *
 * Pure C state machine: consumes raw bytes, emits typed events.
 * No screen state, no allocations, no ESP-IDF dependency.
 *
 * Based on the Paul Williams DEC/ANSI state diagram:
 *   https://vt100.net/emu/dec_ansi_parser
 *
 * UTF-8 note: This parser operates in UTF-8 mode.  Multi-byte UTF-8
 * sequences are decoded only in the GROUND state (printable output).
 * All VT control sequences use 7-bit ASCII bytes exclusively.
 * 8-bit C1 shortcuts (0x80–0x9F) are not used; only 0x9C (ST) is
 * recognised in string states for termination.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Maximum number of CSI/DCS parameters (sub-params via ':' share slots). */
#define VTP_PARAMS_MAX  16

/* Maximum bytes collected into the OSC string buffer (truncated beyond). */
#define VTP_OSC_MAX     256

/* Maximum codepoints buffered before a VT_EV_PRINT span is flushed. */
#define VTP_PRINT_BUF   64

/* ── Per-type callback vtable ─────────────────────────────────────────────── */

typedef struct {
    /* Printable span: cps[0..ncp-1] are codepoints (valid only during cb). */
    void (*print)(const uint32_t *cps, int ncp, void *user);
    /* C0 control byte. */
    void (*c0)   (uint8_t byte, void *user);
    /* ESC sequence. */
    void (*esc)  (uint8_t intermediate, uint8_t final, void *user);
    /* CSI sequence.  params/nparams point directly into the parser buffer —
     * no copy; valid only during the callback. */
    void (*csi)  (uint8_t prefix, uint8_t intermediate, uint8_t final,
                  const int32_t *params, int nparams, void *user);
    /* OSC string.  data is null-terminated; len excludes the terminator. */
    void (*osc)  (const uint8_t *data, int len, void *user);
    /* DCS hook (stub).  params point into the parser buffer. */
    void (*dcs)  (uint8_t prefix, uint8_t intermediate, uint8_t final,
                  const int32_t *params, int nparams, void *user);
} vt_callbacks_t;
/* All slots MUST be non-NULL.  Use a silent no-op for unused event types. */

/* ── Parser state ─────────────────────────────────────────────────────────── */

typedef enum {
    VTP_ST_GROUND = 0,
    VTP_ST_ESC,
    VTP_ST_ESC_INT,
    VTP_ST_CSI_ENTRY,
    VTP_ST_CSI_PARAM,
    VTP_ST_CSI_INT,
    VTP_ST_CSI_IGNORE,
    VTP_ST_DCS_ENTRY,
    VTP_ST_DCS_PARAM,
    VTP_ST_DCS_INT,
    VTP_ST_DCS_PASS,
    VTP_ST_DCS_IGNORE,
    VTP_ST_OSC_STRING,
    VTP_ST_SOS_PM_APC,
} vtp_state_t;

/* prev_string: context saved when ESC interrupts a string-collecting state */
#define VTP_STR_NONE    0
#define VTP_STR_OSC     1
#define VTP_STR_DCS     2
#define VTP_STR_SOS     3   /* SOS / PM / APC — ESC \ terminates silently */

/* ── Parser struct ────────────────────────────────────────────────────────── */

typedef struct {
    vtp_state_t    state;
    uint8_t        prev_string;     /* VTP_STR_* saved context for ESC \     */

    /* UTF-8 decoder */
    uint32_t       utf8_cp;         /* accumulator                           */
    int            utf8_remain;     /* continuation bytes still expected     */

    /* Sequence parameters */
    uint8_t        intermediate;    /* first collected intermediate byte     */
    uint8_t        prefix;          /* first private-use parameter marker    */
    int32_t        params[VTP_PARAMS_MAX];
    int            nparams;         /* slots used so far                     */
    int            param_cur;       /* index of the currently open slot      */

    /* OSC string buffer (+1 for a null terminator written on emit) */
    uint8_t        osc_buf[VTP_OSC_MAX + 1];
    int            osc_len;

    /* Print span buffer */
    uint32_t       print_buf[VTP_PRINT_BUF];
    int            print_len;

    /* Dispatch vtable (copied in by vtparse_init) */
    vt_callbacks_t cb;
    void          *user;
} vtparse_t;

/* ── Public API ───────────────────────────────────────────────────────────── */

/* Initialise (or re-initialise) a parser.  The struct need not be zeroed
 * beforehand; vtparse_init does a full memset internally.
 * The vtable is copied; the caller's vt_callbacks_t need not remain live. */
void vtparse_init(vtparse_t *p, const vt_callbacks_t *cb, void *user);

/* Feed raw bytes.  May call dispatch zero or more times per byte. */
void vtparse_feed(vtparse_t *p, const uint8_t *data, size_t len);
