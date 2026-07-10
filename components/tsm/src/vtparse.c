/*
 * vtparse.c — VT byte-stream parser
 *
 * Paul Williams state machine + UTF-8 decoder.
 * No heap allocations; no ESP-IDF dependency; ISR-safe to call from any
 * context (uses only the vtparse_t on the caller's stack/data segment).
 *
 * SPDX-License-Identifier: MIT
 */

#include "vtparse.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#else
#define IRAM_ATTR
#define likely(x) x
#define unlikely(x) x
#endif

/* ── Internal helpers ─────────────────────────────────────────────────────── */

/* Reset parameter state (called on entry to ESC, CSI, DCS). */
static inline void do_clear(vtparse_t *p)
{
    p->intermediate = 0;
    p->prefix       = 0;
    p->nparams      = 0;
    p->param_cur    = 0;
    for (int i = 0; i < VTP_PARAMS_MAX; i++)
        p->params[i] = -1;
}

/* Accumulate a parameter byte (digit, ':', ';').
 * ':' and ';' both advance the slot so sub-params share the same array. */
static inline void do_param(vtparse_t *p, uint8_t b)
{
    if (b == ';' || b == ':') {
        if (p->param_cur < VTP_PARAMS_MAX - 1)
            p->param_cur++;
        /* Update nparams to include the new slot. */
        if (p->param_cur + 1 > p->nparams)
            p->nparams = p->param_cur + 1;
        return;
    }
    /* Digit 0–9 */
    if (p->param_cur >= VTP_PARAMS_MAX)
        return;
    if (p->params[p->param_cur] < 0)
        p->params[p->param_cur] = b - '0';
    else
        p->params[p->param_cur] = p->params[p->param_cur] * 10 + (b - '0');
    if (p->param_cur + 1 > p->nparams)
        p->nparams = p->param_cur + 1;
}

/* Store first intermediate byte; ignore subsequent ones (rare). */
static inline void do_collect(vtparse_t *p, uint8_t b)
{
    if (likely(!p->intermediate))
        p->intermediate = b;
}

/* Store first private-use parameter marker; ignore subsequent ones. */
static inline void do_prefix(vtparse_t *p, uint8_t b)
{
    if (likely(!p->prefix))
        p->prefix = b;
}

/* ── Event emitters ───────────────────────────────────────────────────────── */

static inline void flush_print(vtparse_t *p)
{
    int n = p->print_len;
    if (n == 0) return;
    p->print_len = 0;
    p->cb.print(p->print_buf, n, p->user);
}

static inline void append_print(vtparse_t *p, uint32_t cp)
{
    if (unlikely(p->print_len >= VTP_PRINT_BUF))
        flush_print(p);
    p->print_buf[p->print_len++] = cp;
}

static inline void emit_c0(vtparse_t *p, uint8_t b)
{
    p->cb.c0(b, p->user);
}

static inline void emit_esc(vtparse_t *p, uint8_t final)
{
    p->cb.esc(p->intermediate, final, p->user);
}

static inline void emit_csi(vtparse_t *p, uint8_t final)
{
    p->cb.csi(p->prefix, p->intermediate, final, p->params, p->nparams, p->user);
}

static inline void emit_osc(vtparse_t *p)
{
    p->osc_buf[p->osc_len] = 0; /* null-terminate */
    p->cb.osc(p->osc_buf, p->osc_len, p->user);
}

static inline void emit_dcs(vtparse_t *p, uint8_t final)
{
    p->cb.dcs(p->prefix, p->intermediate, final, p->params, p->nparams, p->user);
}

/* ── State transitions ────────────────────────────────────────────────────── */

static inline void enter_ground(vtparse_t *p)
{
    p->state       = VTP_ST_GROUND;
    p->prev_string = VTP_STR_NONE;
}

/* Transition to ESC state from any state.
 *
 * Preserves prev_string when interrupting an OSC or DCS string so that
 * a subsequent '\\' (ST) can complete the string correctly.
 */
static inline void enter_esc(vtparse_t *p)
{
    switch (p->state) {
        case VTP_ST_OSC_STRING:
            p->prev_string = VTP_STR_OSC;
            break;
        case VTP_ST_DCS_ENTRY:
        case VTP_ST_DCS_PARAM:
        case VTP_ST_DCS_INT:
        case VTP_ST_DCS_PASS:
        case VTP_ST_DCS_IGNORE:
            p->prev_string = VTP_STR_DCS;
            break;
        case VTP_ST_SOS_PM_APC:
            p->prev_string = VTP_STR_SOS;
            break;
        case VTP_ST_ESC:
        case VTP_ST_ESC_INT:
            /* Second ESC while already in ESC: keep existing prev_string. */
            break;
        default:
            p->prev_string = VTP_STR_NONE;
            break;
    }
    do_clear(p);
    p->state = VTP_ST_ESC;
}

/* ── Per-state byte processors ────────────────────────────────────────────── */

static inline void st_ground(vtparse_t *p, uint8_t b)
{
    if (b <= 0x1F) {
        flush_print(p);
        emit_c0(p, b);          /* C0 controls; ESC handled by "anywhere" */
    } else if (likely(b <= 0x7E)) {
        append_print(p, b);     /* printable ASCII */
    }
    /* 0x7F (DEL) → ignore */
}

static inline void st_esc(vtparse_t *p, uint8_t b)
{
    if (b >= 0x20 && b <= 0x2F) {
        /* Intermediate byte (e.g. '(' for charset designation) */
        do_collect(p, b);
        p->state = VTP_ST_ESC_INT;
    } else if (b == 0x5B) {         /* '[' → CSI */
        do_clear(p);
        p->prev_string = VTP_STR_NONE;
        p->state = VTP_ST_CSI_ENTRY;
    } else if (b == 0x5D) {         /* ']' → OSC */
        p->osc_len     = 0;
        p->prev_string = VTP_STR_NONE;
        p->state       = VTP_ST_OSC_STRING;
    } else if (b == 0x50) {         /* 'P' → DCS */
        do_clear(p);
        p->prev_string = VTP_STR_NONE;
        p->state = VTP_ST_DCS_ENTRY;
    } else if (b == 0x58 || b == 0x5E || b == 0x5F) { /* 'X' '^' '_' */
        /* SOS / PM / APC — ignore until ST */
        p->prev_string = VTP_STR_NONE;
        p->state       = VTP_ST_SOS_PM_APC;
    } else if (b >= 0x30 && b <= 0x7E) {
        /* Standard ESC final byte.  Check for ST = ESC \ (0x5C). */
        if (b == 0x5C) {
            if (p->prev_string == VTP_STR_OSC) {
                emit_osc(p);
            } else if (p->prev_string == VTP_STR_DCS) {
                emit_dcs(p, b);
            } else if (p->prev_string == VTP_STR_SOS) {
                /* SOS/PM/APC content is fully ignored; consume ST silently. */
            } else {
                emit_esc(p, b);
            }
        } else {
            emit_esc(p, b);
        }
        enter_ground(p);
    }
    /* 0x7F → ignore */
}

static inline void st_esc_int(vtparse_t *p, uint8_t b)
{
    if (b >= 0x20 && b <= 0x2F) {
        do_collect(p, b);       /* additional intermediate (unusual) */
    } else if (b >= 0x30 && b <= 0x7E) {
        /* ESC \ while we came from OSC: treat as string terminator */
        if (b == 0x5C && p->prev_string == VTP_STR_OSC) {
            emit_osc(p);
        } else {
            emit_esc(p, b);
        }
        enter_ground(p);
    }
    /* 0x7F → ignore */
}

static inline  void st_csi_entry(vtparse_t *p, uint8_t b)
{
    if (b >= 0x20 && b <= 0x2F) {
        do_collect(p, b);
        p->state = VTP_ST_CSI_INT;
    } else if (b >= 0x30 && b <= 0x39) {   /* digit */
        do_param(p, b);
        p->state = VTP_ST_CSI_PARAM;
    } else if (b == 0x3A || b == 0x3B) {   /* ':' or ';' */
        do_param(p, b);
        p->state = VTP_ST_CSI_PARAM;
    } else if (b >= 0x3C && b <= 0x3F) {   /* '<' '=' '>' '?' */
        do_prefix(p, b);
        p->state = VTP_ST_CSI_PARAM;
    } else if (b >= 0x40 && b <= 0x7E) {   /* final byte */
        emit_csi(p, b);
        enter_ground(p);
    }
    /* 0x7F → ignore */
}

static inline void st_csi_param(vtparse_t *p, uint8_t b)
{
    if (b >= 0x20 && b <= 0x2F) {
        do_collect(p, b);
        p->state = VTP_ST_CSI_INT;
    } else if ((b >= 0x30 && b <= 0x39) || b == 0x3A || b == 0x3B) {
        do_param(p, b);
    } else if (b >= 0x3C && b <= 0x3F) {
        /* Second private marker after digits → malformed → ignore. */
        p->state = VTP_ST_CSI_IGNORE;
    } else if (b >= 0x40 && b <= 0x7E) {
        emit_csi(p, b);
        enter_ground(p);
    }
    /* 0x7F → ignore */
}

static inline void st_csi_int(vtparse_t *p, uint8_t b)
{
    if (b >= 0x20 && b <= 0x2F) {
        do_collect(p, b);
    } else if (b >= 0x30 && b <= 0x3F) {
        /* Parameter byte after intermediate → malformed → ignore. */
        p->state = VTP_ST_CSI_IGNORE;
    } else if (b >= 0x40 && b <= 0x7E) {
        emit_csi(p, b);
        enter_ground(p);
    }
    /* 0x7F → ignore */
}

static inline void st_csi_ignore(vtparse_t *p, uint8_t b)
{
    if (b >= 0x40 && b <= 0x7E)
        enter_ground(p);    /* discard sequence, return to ground */
    /* everything else: stay and discard */
}

static inline void st_dcs_entry(vtparse_t *p, uint8_t b)
{
    if (b >= 0x20 && b <= 0x2F) {
        do_collect(p, b);
        p->state = VTP_ST_DCS_INT;
    } else if ((b >= 0x30 && b <= 0x39) || b == 0x3B) {
        do_param(p, b);
        p->state = VTP_ST_DCS_PARAM;
    } else if (b == 0x3A || (b >= 0x3C && b <= 0x3F)) {
        p->state = VTP_ST_DCS_IGNORE;
    } else if (b >= 0x40 && b <= 0x7E) {
        /* DCS hook: emit stub event then collect passthrough data. */
        emit_dcs(p, b);
        p->state = VTP_ST_DCS_PASS;
    }
}

static inline void st_dcs_param(vtparse_t *p, uint8_t b)
{
    if (b >= 0x20 && b <= 0x2F) {
        do_collect(p, b);
        p->state = VTP_ST_DCS_INT;
    } else if ((b >= 0x30 && b <= 0x39) || b == 0x3B) {
        do_param(p, b);
    } else if (b == 0x3A || (b >= 0x3C && b <= 0x3F)) {
        p->state = VTP_ST_DCS_IGNORE;
    } else if (b >= 0x40 && b <= 0x7E) {
        emit_dcs(p, b);
        p->state = VTP_ST_DCS_PASS;
    }
}

static inline void st_dcs_int(vtparse_t *p, uint8_t b)
{
    if (b >= 0x20 && b <= 0x2F) {
        do_collect(p, b);
    } else if (b >= 0x30 && b <= 0x3F) {
        p->state = VTP_ST_DCS_IGNORE;
    } else if (b >= 0x40 && b <= 0x7E) {
        emit_dcs(p, b);
        p->state = VTP_ST_DCS_PASS;
    }
}

/* DCS passthrough data (Phase 1 stub: data bytes are silently consumed). */
static inline  void st_dcs_pass(vtparse_t *p, uint8_t b)
{
    (void)p;
    (void)b;
    /* Terminated by 0x9C or ESC \ via the "anywhere" transitions. */
}

static inline void st_dcs_ignore(vtparse_t *p, uint8_t b)
{
    (void)p;
    (void)b;
    /* Ignore everything until ST. */
}

static inline void st_osc(vtparse_t *p, uint8_t b)
{
    if (b == 0x07) {
        /* BEL — xterm extension for OSC termination. */
        emit_osc(p);
        enter_ground(p);
    } else if (b >= 0x20 && b <= 0x7E) {
        /* Collect printable ASCII bytes (7-bit only; UTF-8 in titles is
         * ignored in Phase 1 — high bytes don't reach this function). */
        if (p->osc_len < VTP_OSC_MAX)
            p->osc_buf[p->osc_len++] = b;
        /* Bytes beyond VTP_OSC_MAX are silently dropped. */
    }
    /* Other C0 (except BEL already handled above) → ignore. */
}

static inline void st_sos_pm_apc(vtparse_t *p, uint8_t b)
{
    (void)p;
    (void)b;
    /* Ignore everything until ST (ESC \ or 0x9C). */
}

/* ── UTF-8 lead byte handler ──────────────────────────────────────────────── */

/* Begin accumulating a multi-byte UTF-8 sequence from lead byte b.
 * Returns true on a valid lead; false if b should produce U+FFFD. */
static inline bool utf8_start(vtparse_t *p, uint8_t b)
{
    if (unlikely(b >= 0xF5)) {
        return false;       /* would exceed U+10FFFF */
    }
    if (b >= 0xF0) {
        p->utf8_cp     = b & 0x07u;
        p->utf8_remain = 3;
    } else if (b >= 0xE0) {
        p->utf8_cp     = b & 0x0Fu;
        p->utf8_remain = 2;
    } else if (b >= 0xC2) {
        p->utf8_cp     = b & 0x1Fu;
        p->utf8_remain = 1;
    } else {
        return false;       /* 0xC0, 0xC1: overlong encoding */
    }
    return true;
}

/* ── Public API ───────────────────────────────────────────────────────────── */

void vtparse_init(vtparse_t *p, const vt_callbacks_t *cb, void *user)
{
    memset(p, 0, sizeof(*p));
    p->cb    = *cb;
    p->user  = user;
    p->state = VTP_ST_GROUND;
    for (int i = 0; i < VTP_PARAMS_MAX; i++)
        p->params[i] = -1;
}

void IRAM_ATTR vtparse_feed(vtparse_t *p, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        /* ── UTF-8 continuation ─────────────────────────────────────────── */
        if (p->utf8_remain > 0) {
            if ((b & 0xC0u) == 0x80u) {
                /* Valid continuation byte. */
                p->utf8_cp = (p->utf8_cp << 6) | (b & 0x3Fu);
                if (--p->utf8_remain == 0) {
                    uint32_t cp = p->utf8_cp;
                    /* Reject surrogates and out-of-BMP (unsupported font). */
                    if (unlikely(cp > 0x10FFFFu ||
                        (cp >= 0xD800u && cp <= 0xDFFFu) ||
                        cp > 0xFFFFu)) {
                        cp = 0xFFFDu;
                    }
                    if (likely(p->state == VTP_ST_GROUND))
                        append_print(p, cp);
                }
                continue;
            }
            /* Not a continuation byte: cancel the sequence → U+FFFD. */
            if (p->state == VTP_ST_GROUND)
                append_print(p, 0xFFFDu);
            p->utf8_remain = 0;
            p->utf8_cp     = 0;
            /* Fall through to process the current byte normally. */
        }

        /* ── "Anywhere" transitions (ASCII control only, UTF-8 mode) ────── */
        if (b == 0x18 || b == 0x1A) {
            flush_print(p);
            emit_c0(p, b);
            enter_ground(p);
            continue;
        }
        if (b == 0x1B) {
            flush_print(p);
            enter_esc(p);
            continue;
        }

        /* ── High bytes (0x80–0xFF) ─────────────────────────────────────── */
        if (b >= 0x80u) {
            /* 0x9C: 8-bit String Terminator — terminates OSC/DCS. */
            if (b == 0x9Cu) {
                flush_print(p);
                if (p->state == VTP_ST_OSC_STRING)
                    emit_osc(p);
                else if (p->state == VTP_ST_DCS_PASS ||
                         p->state == VTP_ST_DCS_IGNORE)
                    emit_dcs(p, 0x5Cu); /* report as ESC \ equivalent */
                enter_ground(p);
                continue;
            }
            /* UTF-8 lead bytes are valid only in GROUND state. */
            if (p->state == VTP_ST_GROUND) {
                if (b >= 0xC2u && b <= 0xF4u) {
                    if (utf8_start(p, b))
                        continue;
                }
                /* Invalid lead or out-of-range → replacement character. */
                append_print(p, 0xFFFDu);
            }
            /* In non-GROUND states, unexpected high bytes are discarded. */
            continue;
        }

        /* ── Normal state dispatch (b is 0x00–0x7F) ─────────────────────── */
        switch (p->state) {
            case VTP_ST_GROUND:       st_ground(p, b);      break;
            case VTP_ST_ESC:          st_esc(p, b);         break;
            case VTP_ST_ESC_INT:      st_esc_int(p, b);     break;
            case VTP_ST_CSI_ENTRY:    st_csi_entry(p, b);   break;
            case VTP_ST_CSI_PARAM:    st_csi_param(p, b);   break;
            case VTP_ST_CSI_INT:      st_csi_int(p, b);     break;
            case VTP_ST_CSI_IGNORE:   st_csi_ignore(p, b);  break;
            case VTP_ST_DCS_ENTRY:    st_dcs_entry(p, b);   break;
            case VTP_ST_DCS_PARAM:    st_dcs_param(p, b);   break;
            case VTP_ST_DCS_INT:      st_dcs_int(p, b);     break;
            case VTP_ST_DCS_PASS:     st_dcs_pass(p, b);    break;
            case VTP_ST_DCS_IGNORE:   st_dcs_ignore(p, b);  break;
            case VTP_ST_OSC_STRING:   st_osc(p, b);         break;
            case VTP_ST_SOS_PM_APC:   st_sos_pm_apc(p, b);  break;
            default:                  break;
        }
    }
    flush_print(p);
}
