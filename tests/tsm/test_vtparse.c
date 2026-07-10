/*
 * Unit tests for vtparse — VT byte-stream parser.
 *
 * Build & run:
 *   cd tests/tsm
 *   cmake -B build && cmake --build build
 *   ./build/test_vtparse
 *
 * Coverage:
 *   - C0 controls
 *   - ESC sequences (with and without intermediates)
 *   - CSI: params, defaults, prefix markers, sub-params, intermediates
 *   - OSC: BEL-terminated, ESC \-terminated, truncation
 *   - DCS: entry and ESC \-termination stub
 *   - UTF-8: 2-byte, 3-byte, 4-byte (→ U+FFFD), split feeds,
 *             stray continuation, interrupted by ESC, invalid lead
 *   - State recovery: ESC interrupts CSI, 0x18/0x1A reset to ground
 */

#include "unity.h"
#include "vtparse.h"
#include <string.h>
#include <stdint.h>

/* ── Event recorder ───────────────────────────────────────────────────────── */

#define MAX_EVENTS  64

/* Local event type enum (mirrors the vtable slot names). */
typedef enum {
    VT_EV_PRINT, VT_EV_C0, VT_EV_ESC, VT_EV_CSI, VT_EV_OSC, VT_EV_DCS
} rec_ev_type_t;

/* Discriminated-union recorded event — only the fields each type carries. */
typedef struct {
    rec_ev_type_t type;
    union {
        struct { const uint32_t *cps; int ncp; }                    as_print;
        struct { uint8_t byte; }                                    as_c0;
        struct { uint8_t intermediate; uint8_t final; }             as_esc;
        struct { uint8_t prefix; uint8_t intermediate; uint8_t final;
                 int32_t params[VTP_PARAMS_MAX]; int nparams; }     as_csi;
        struct { const uint8_t *osc; int osc_len; }                 as_osc;
        struct { uint8_t prefix; uint8_t intermediate; uint8_t final;
                 int32_t params[VTP_PARAMS_MAX]; int nparams; }     as_dcs;
    };
} recorded_event_t;

static vtparse_t      g_parser;
static recorded_event_t g_events[MAX_EVENTS];
static int            g_event_count;

/* Separate storage for OSC data (the parser's buffer is reused). */
static uint8_t        g_osc_store[MAX_EVENTS][VTP_OSC_MAX + 1];
static int            g_osc_idx;

/* Separate storage for PRINT span data (the parser's print_buf is reused). */
static uint32_t       g_print_store[MAX_EVENTS][VTP_PRINT_BUF];
static int            g_print_idx;

static void rec_print(const uint32_t *cps, int ncp, void *user)
{
    (void)user;
    if (g_event_count >= MAX_EVENTS) return;
    recorded_event_t *e = &g_events[g_event_count++];
    e->type = VT_EV_PRINT;
    memcpy(g_print_store[g_print_idx], cps, (size_t)ncp * sizeof(uint32_t));
    e->as_print.cps = g_print_store[g_print_idx++];
    e->as_print.ncp = ncp;
}
static void rec_c0(uint8_t byte, void *user)
{
    (void)user;
    if (g_event_count >= MAX_EVENTS) return;
    recorded_event_t *e = &g_events[g_event_count++];
    e->type = VT_EV_C0;
    e->as_c0.byte = byte;
}
static void rec_esc(uint8_t intermediate, uint8_t final, void *user)
{
    (void)user;
    if (g_event_count >= MAX_EVENTS) return;
    recorded_event_t *e = &g_events[g_event_count++];
    e->type = VT_EV_ESC;
    e->as_esc.intermediate = intermediate;
    e->as_esc.final = final;
}
static void rec_csi(uint8_t prefix, uint8_t intermediate, uint8_t final,
                    const int32_t *params, int nparams, void *user)
{
    (void)user;
    if (g_event_count >= MAX_EVENTS) return;
    recorded_event_t *e = &g_events[g_event_count++];
    e->type = VT_EV_CSI;
    e->as_csi.prefix = prefix;
    e->as_csi.intermediate = intermediate;
    e->as_csi.final = final;
    e->as_csi.nparams = nparams;
    for (int i = 0; i < nparams && i < VTP_PARAMS_MAX; i++)
        e->as_csi.params[i] = params[i];
}
static void rec_osc(const uint8_t *data, int len, void *user)
{
    (void)user;
    if (g_event_count >= MAX_EVENTS) return;
    recorded_event_t *e = &g_events[g_event_count++];
    e->type = VT_EV_OSC;
    memcpy(g_osc_store[g_osc_idx], data, (size_t)len + 1);
    e->as_osc.osc = g_osc_store[g_osc_idx++];
    e->as_osc.osc_len = len;
}
static void rec_dcs(uint8_t prefix, uint8_t intermediate, uint8_t final,
                    const int32_t *params, int nparams, void *user)
{
    (void)user;
    if (g_event_count >= MAX_EVENTS) return;
    recorded_event_t *e = &g_events[g_event_count++];
    e->type = VT_EV_DCS;
    e->as_dcs.prefix = prefix;
    e->as_dcs.intermediate = intermediate;
    e->as_dcs.final = final;
    e->as_dcs.nparams = nparams;
    for (int i = 0; i < nparams && i < VTP_PARAMS_MAX; i++)
        e->as_dcs.params[i] = params[i];
}

static const vt_callbacks_t g_rec_cb = {
    .print = rec_print, .c0 = rec_c0, .esc = rec_esc,
    .csi   = rec_csi,   .osc = rec_osc, .dcs = rec_dcs,
};

/* Convenience: feed a C string (without null terminator). */
static void feed_str(const char *s)
{
    vtparse_feed(&g_parser, (const uint8_t *)s, strlen(s));
}

/* Convenience: feed a raw byte array. */
static void feed_bytes(const uint8_t *b, size_t n)
{
    vtparse_feed(&g_parser, b, n);
}

void setUp(void)
{
    memset(g_events,      0, sizeof(g_events));
    memset(g_osc_store,   0, sizeof(g_osc_store));
    memset(g_print_store, 0, sizeof(g_print_store));
    g_event_count = 0;
    g_osc_idx     = 0;
    g_print_idx   = 0;
    vtparse_init(&g_parser, &g_rec_cb, NULL);
}

void tearDown(void) {}


/* ════════════════════════════════════════════════════════════════════════════
 * C0 controls
 * ══════════════════════════════════════════════════════════════════════════ */

void test_c0_bel(void)
{
    uint8_t b = 0x07;
    feed_bytes(&b, 1);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_C0, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT8(0x07, g_events[0].as_c0.byte);
}

void test_c0_bs(void)
{
    uint8_t b = 0x08;
    feed_bytes(&b, 1);
    TEST_ASSERT_EQUAL_INT(VT_EV_C0, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT8(0x08, g_events[0].as_c0.byte);
}

void test_c0_lf_cr(void)
{
    uint8_t bs[] = { 0x0A, 0x0D };
    feed_bytes(bs, 2);
    TEST_ASSERT_EQUAL_INT(2, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_C0, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT8(0x0A, g_events[0].as_c0.byte);
    TEST_ASSERT_EQUAL_INT(VT_EV_C0, g_events[1].type);
    TEST_ASSERT_EQUAL_UINT8(0x0D, g_events[1].as_c0.byte);
}

void test_c0_so_si(void)
{
    uint8_t bs[] = { 0x0E, 0x0F };
    feed_bytes(bs, 2);
    TEST_ASSERT_EQUAL_INT(2, g_event_count);
    TEST_ASSERT_EQUAL_UINT8(0x0E, g_events[0].as_c0.byte);
    TEST_ASSERT_EQUAL_UINT8(0x0F, g_events[1].as_c0.byte);
}

void test_del_ignored(void)
{
    /* DEL (0x7F) must produce no event. */
    uint8_t b = 0x7F;
    feed_bytes(&b, 1);
    TEST_ASSERT_EQUAL_INT(0, g_event_count);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Printable ASCII
 * ══════════════════════════════════════════════════════════════════════════ */

void test_printable_ascii(void)
{
    feed_str("Hi");
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_PRINT, g_events[0].type);
    TEST_ASSERT_EQUAL_INT(2, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32('H', g_events[0].as_print.cps[0]);
    TEST_ASSERT_EQUAL_UINT32('i', g_events[0].as_print.cps[1]);
}

/* ════════════════════════════════════════════════════════════════════════════
 * ESC sequences
 * ══════════════════════════════════════════════════════════════════════════ */

void test_esc_D(void)
{
    /* ESC D = IND (index) */
    uint8_t bs[] = { 0x1B, 'D' };
    feed_bytes(bs, 2);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_ESC, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT8('D', g_events[0].as_esc.final);
    TEST_ASSERT_EQUAL_UINT8(0,   g_events[0].as_esc.intermediate);
}

void test_esc_M(void)
{
    /* ESC M = RI (reverse index) */
    uint8_t bs[] = { 0x1B, 'M' };
    feed_bytes(bs, 2);
    TEST_ASSERT_EQUAL_INT(VT_EV_ESC, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT8('M', g_events[0].as_esc.final);
}

void test_esc_7_8(void)
{
    /* ESC 7 = DECSC, ESC 8 = DECRC */
    uint8_t bs[] = { 0x1B, '7', 0x1B, '8' };
    feed_bytes(bs, 4);
    TEST_ASSERT_EQUAL_INT(2, g_event_count);
    TEST_ASSERT_EQUAL_UINT8('7', g_events[0].as_esc.final);
    TEST_ASSERT_EQUAL_UINT8('8', g_events[1].as_esc.final);
}

void test_esc_eq_gt(void)
{
    /* ESC = DECKPAM, ESC > DECKPNM */
    uint8_t bs[] = { 0x1B, '=', 0x1B, '>' };
    feed_bytes(bs, 4);
    TEST_ASSERT_EQUAL_INT(2, g_event_count);
    TEST_ASSERT_EQUAL_UINT8('=', g_events[0].as_esc.final);
    TEST_ASSERT_EQUAL_UINT8('>', g_events[1].as_esc.final);
}

void test_esc_with_intermediate(void)
{
    /* ESC ( B = designate G0 as ASCII */
    uint8_t bs[] = { 0x1B, '(', 'B' };
    feed_bytes(bs, 3);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_ESC, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT8('(', g_events[0].as_esc.intermediate);
    TEST_ASSERT_EQUAL_UINT8('B', g_events[0].as_esc.final);
}

void test_esc_charset_dec_special(void)
{
    /* ESC ( 0 = designate G0 as DEC Special Graphics */
    uint8_t bs[] = { 0x1B, '(', '0' };
    feed_bytes(bs, 3);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_UINT8('(', g_events[0].as_esc.intermediate);
    TEST_ASSERT_EQUAL_UINT8('0', g_events[0].as_esc.final);
}

/* ════════════════════════════════════════════════════════════════════════════
 * CSI sequences
 * ══════════════════════════════════════════════════════════════════════════ */

void test_csi_no_params(void)
{
    /* ESC [ H — cursor home, no params */
    uint8_t bs[] = { 0x1B, '[', 'H' };
    feed_bytes(bs, 3);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_CSI, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT8('H', g_events[0].as_csi.final);
    TEST_ASSERT_EQUAL_UINT8(0,   g_events[0].as_csi.nparams);
    TEST_ASSERT_EQUAL_UINT8(0,   g_events[0].as_csi.prefix);
}

void test_csi_one_param(void)
{
    /* ESC [ 5 H — cursor to row 5 */
    uint8_t bs[] = { 0x1B, '[', '5', 'H' };
    feed_bytes(bs, 4);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_UINT8(1, g_events[0].as_csi.nparams);
    TEST_ASSERT_EQUAL_INT32(5, g_events[0].as_csi.params[0]);
}

void test_csi_two_params(void)
{
    /* ESC [ 2 ; 5 H — cursor to row 2, col 5 */
    uint8_t bs[] = { 0x1B, '[', '2', ';', '5', 'H' };
    feed_bytes(bs, 6);
    TEST_ASSERT_EQUAL_UINT8(2, g_events[0].as_csi.nparams);
    TEST_ASSERT_EQUAL_INT32(2, g_events[0].as_csi.params[0]);
    TEST_ASSERT_EQUAL_INT32(5, g_events[0].as_csi.params[1]);
}

void test_csi_default_first_param(void)
{
    /* ESC [ ; 5 H — first param omitted (default), col = 5 */
    uint8_t bs[] = { 0x1B, '[', ';', '5', 'H' };
    feed_bytes(bs, 5);
    TEST_ASSERT_EQUAL_UINT8(2, g_events[0].as_csi.nparams);
    TEST_ASSERT_EQUAL_INT32(-1, g_events[0].as_csi.params[0]);  /* default */
    TEST_ASSERT_EQUAL_INT32(5,  g_events[0].as_csi.params[1]);
}

void test_csi_default_only(void)
{
    /* ESC [ m — SGR reset, no params at all */
    uint8_t bs[] = { 0x1B, '[', 'm' };
    feed_bytes(bs, 3);
    TEST_ASSERT_EQUAL_UINT8(0, g_events[0].as_csi.nparams);
}

void test_csi_private_question_mark(void)
{
    /* ESC [ ? 2 5 h — DECTCEM set */
    uint8_t bs[] = { 0x1B, '[', '?', '2', '5', 'h' };
    feed_bytes(bs, 6);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_UINT8('?', g_events[0].as_csi.prefix);
    TEST_ASSERT_EQUAL_UINT8('h', g_events[0].as_csi.final);
    TEST_ASSERT_EQUAL_INT32(25,  g_events[0].as_csi.params[0]);
}

void test_csi_private_gt(void)
{
    /* ESC [ > 1 m — secondary DA response modifier */
    uint8_t bs[] = { 0x1B, '[', '>', '1', 'm' };
    feed_bytes(bs, 5);
    TEST_ASSERT_EQUAL_UINT8('>', g_events[0].as_csi.prefix);
    TEST_ASSERT_EQUAL_UINT8('m', g_events[0].as_csi.final);
    TEST_ASSERT_EQUAL_INT32(1,   g_events[0].as_csi.params[0]);
}

void test_csi_intermediate_bang(void)
{
    /* ESC [ ! p — DECSTR (soft terminal reset) */
    uint8_t bs[] = { 0x1B, '[', '!', 'p' };
    feed_bytes(bs, 4);
    TEST_ASSERT_EQUAL_UINT8('!', g_events[0].as_csi.intermediate);
    TEST_ASSERT_EQUAL_UINT8('p', g_events[0].as_csi.final);
    TEST_ASSERT_EQUAL_UINT8(0,   g_events[0].as_csi.prefix);
}

void test_csi_sub_params_colon(void)
{
    /* ESC [ 3 8 : 2 : 2 5 5 : 0 : 0 m — truecolor fg via sub-params */
    uint8_t bs[] = { 0x1B, '[', '3','8', ':', '2', ':', '2','5','5',
                     ':', '0', ':', '0', 'm' };
    feed_bytes(bs, sizeof(bs));
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_UINT8('m', g_events[0].as_csi.final);
    TEST_ASSERT_EQUAL_INT32(38,  g_events[0].as_csi.params[0]);
    TEST_ASSERT_EQUAL_INT32(2,   g_events[0].as_csi.params[1]);
    TEST_ASSERT_EQUAL_INT32(255, g_events[0].as_csi.params[2]);
    TEST_ASSERT_EQUAL_INT32(0,   g_events[0].as_csi.params[3]);
    TEST_ASSERT_EQUAL_INT32(0,   g_events[0].as_csi.params[4]);
}

void test_csi_truecolor_semicolons(void)
{
    /* ESC [ 3 8 ; 2 ; 1 ; 2 ; 3 m — truecolor fg via standard semicolons */
    uint8_t bs[] = { 0x1B, '[', '3','8', ';', '2', ';', '1', ';', '2', ';', '3', 'm' };
    feed_bytes(bs, sizeof(bs));
    TEST_ASSERT_EQUAL_INT32(38, g_events[0].as_csi.params[0]);
    TEST_ASSERT_EQUAL_INT32(2,  g_events[0].as_csi.params[1]);
    TEST_ASSERT_EQUAL_INT32(1,  g_events[0].as_csi.params[2]);
    TEST_ASSERT_EQUAL_INT32(2,  g_events[0].as_csi.params[3]);
    TEST_ASSERT_EQUAL_INT32(3,  g_events[0].as_csi.params[4]);
    TEST_ASSERT_EQUAL_UINT8(5,  g_events[0].as_csi.nparams);
}

void test_csi_max_params(void)
{
    /* 16 params — should all be captured */
    uint8_t bs[] = { 0x1B, '[',
        '1',';','2',';','3',';','4',';','5',';','6',';','7',';','8',';',
        '9',';','1','0',';','1','1',';','1','2',';','1','3',';','1','4',
        ';','1','5',';','1','6', 'm' };
    feed_bytes(bs, sizeof(bs));
    TEST_ASSERT_EQUAL_UINT8(16, g_events[0].as_csi.nparams);
    TEST_ASSERT_EQUAL_INT32(1,  g_events[0].as_csi.params[0]);
    TEST_ASSERT_EQUAL_INT32(16, g_events[0].as_csi.params[15]);
}

void test_csi_malformed_second_private_marker(void)
{
    /* ESC [ ? 2 ? h — second '?' after digits is malformed → no dispatch */
    uint8_t bs[] = { 0x1B, '[', '?', '2', '?', 'h' };
    feed_bytes(bs, 6);
    TEST_ASSERT_EQUAL_INT(0, g_event_count);
}

void test_csi_multiple_sequences(void)
{
    /* Two CSI sequences back-to-back */
    uint8_t bs[] = { 0x1B,'[','1','m', 0x1B,'[','0','m' };
    feed_bytes(bs, 8);
    TEST_ASSERT_EQUAL_INT(2, g_event_count);
    TEST_ASSERT_EQUAL_INT32(1, g_events[0].as_csi.params[0]);
    TEST_ASSERT_EQUAL_INT32(0, g_events[1].as_csi.params[0]);
}

/* ════════════════════════════════════════════════════════════════════════════
 * OSC sequences
 * ══════════════════════════════════════════════════════════════════════════ */

void test_osc_bel_terminated(void)
{
    /* ESC ] 0 ; t i t l e BEL */
    uint8_t bs[] = { 0x1B, ']', '0', ';', 't','i','t','l','e', 0x07 };
    feed_bytes(bs, sizeof(bs));
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_OSC, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT16(7, g_events[0].as_osc.osc_len);  /* "0;title" = 7 bytes */
    TEST_ASSERT_EQUAL_UINT8('0', g_events[0].as_osc.osc[0]);
    TEST_ASSERT_EQUAL_UINT8(';', g_events[0].as_osc.osc[1]);
    TEST_ASSERT_EQUAL_UINT8('t', g_events[0].as_osc.osc[2]);
}

void test_osc_st_terminated(void)
{
    /* ESC ] 0 ; H i ESC \ */
    uint8_t bs[] = { 0x1B, ']', '0', ';', 'H','i', 0x1B, '\\' };
    feed_bytes(bs, sizeof(bs));
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_OSC, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT16(4, g_events[0].as_osc.osc_len);  /* "0;Hi" */
    TEST_ASSERT_EQUAL_UINT8('H', g_events[0].as_osc.osc[2]);
    TEST_ASSERT_EQUAL_UINT8('i', g_events[0].as_osc.osc[3]);
}

void test_osc_null_terminated(void)
{
    /* OSC buffer is always null-terminated; consumer can use osc as C string */
    uint8_t bs[] = { 0x1B, ']', '2', ';', 'A', 0x07 };
    feed_bytes(bs, sizeof(bs));
    TEST_ASSERT_EQUAL_UINT8(0, g_events[0].as_osc.osc[g_events[0].as_osc.osc_len]);
}

void test_osc_truncation(void)
{
    /* Feed VTP_OSC_MAX + 10 bytes into an OSC string — must not overflow. */
    uint8_t buf[VTP_OSC_MAX + 12];
    buf[0] = 0x1B;
    buf[1] = ']';
    for (int i = 2; i < (int)sizeof(buf) - 1; i++)
        buf[i] = 'X';
    buf[sizeof(buf) - 1] = 0x07;   /* BEL terminator */
    feed_bytes(buf, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_OSC, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT16(VTP_OSC_MAX, g_events[0].as_osc.osc_len);  /* capped */
}

void test_osc_8bit_st(void)
{
    /* OSC terminated by 8-bit ST (0x9C) */
    uint8_t bs[] = { 0x1B, ']', '0', ';', 'Z', 0x9C };
    feed_bytes(bs, sizeof(bs));
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_OSC, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT16(3, g_events[0].as_osc.osc_len);  /* "0;Z" */
}

/* ════════════════════════════════════════════════════════════════════════════
 * DCS sequences (Phase 1 stub)
 * ══════════════════════════════════════════════════════════════════════════ */

void test_dcs_hook_emitted(void)
{
    /* ESC P q … ESC \ — tmux-style passthrough shell */
    uint8_t bs[] = { 0x1B, 'P', 'q', 'a','b','c', 0x1B, '\\' };
    feed_bytes(bs, sizeof(bs));
    /* Phase 1: DCS hook fires on the final byte ('q') */
    TEST_ASSERT_GREATER_OR_EQUAL(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_DCS, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT8('q', g_events[0].as_dcs.final);
}

void test_dcs_no_crash_on_data(void)
{
    /* Feed a long DCS string and verify no crash or stray events. */
    uint8_t buf[64];
    buf[0] = 0x1B;
    buf[1] = 'P';
    buf[2] = 'q';                  /* final → DCS hook */
    for (int i = 3; i < 62; i++)
        buf[i] = 'X';              /* passthrough data */
    buf[62] = 0x1B;
    buf[63] = '\\';                /* ST */
    feed_bytes(buf, sizeof(buf));
    /* Must have finished in GROUND state — a subsequent printable is visible */
    g_event_count = 0;
    feed_str("A");
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_PRINT, g_events[0].type);
    TEST_ASSERT_EQUAL_INT(1, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32('A', g_events[0].as_print.cps[0]);
}

/* ════════════════════════════════════════════════════════════════════════════
 * UTF-8 decoding
 * ══════════════════════════════════════════════════════════════════════════ */

void test_utf8_2byte(void)
{
    /* é = U+00E9 → 0xC3 0xA9 */
    uint8_t bs[] = { 0xC3, 0xA9 };
    feed_bytes(bs, 2);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_PRINT, g_events[0].type);
    TEST_ASSERT_EQUAL_INT(1, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32(0x00E9u, g_events[0].as_print.cps[0]);
}

void test_utf8_3byte(void)
{
    /* ╔ = U+2554 → 0xE2 0x95 0x94 */
    uint8_t bs[] = { 0xE2, 0x95, 0x94 };
    feed_bytes(bs, 3);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(1, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32(0x2554u, g_events[0].as_print.cps[0]);
}

void test_utf8_4byte_replaced_with_fffd(void)
{
    /* 😀 = U+1F600 → SMP, unsupported font → U+FFFD */
    uint8_t bs[] = { 0xF0, 0x9F, 0x98, 0x80 };
    feed_bytes(bs, 4);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(1, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32(0xFFFDu, g_events[0].as_print.cps[0]);
}

void test_utf8_split_across_feeds(void)
{
    /* Feed é in two separate calls: first byte, then second byte. */
    uint8_t b1[] = { 0xC3 };
    uint8_t b2[] = { 0xA9 };
    feed_bytes(b1, 1);
    TEST_ASSERT_EQUAL_INT(0, g_event_count);  /* incomplete */
    feed_bytes(b2, 1);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(1, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32(0x00E9u, g_events[0].as_print.cps[0]);
}

void test_utf8_3byte_split_three_feeds(void)
{
    /* Feed ╔ one byte at a time. */
    uint8_t bs[] = { 0xE2, 0x95, 0x94 };
    vtparse_feed(&g_parser, &bs[0], 1);
    TEST_ASSERT_EQUAL_INT(0, g_event_count);
    vtparse_feed(&g_parser, &bs[1], 1);
    TEST_ASSERT_EQUAL_INT(0, g_event_count);
    vtparse_feed(&g_parser, &bs[2], 1);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(1, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32(0x2554u, g_events[0].as_print.cps[0]);
}

void test_utf8_stray_continuation(void)
{
    /* 0x80 alone (continuation without lead) → U+FFFD */
    uint8_t b = 0x80;
    feed_bytes(&b, 1);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(1, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32(0xFFFDu, g_events[0].as_print.cps[0]);
}

void test_utf8_invalid_overlong_c0(void)
{
    /* 0xC0 0x80 would encode U+0000 as overlong → both bytes → U+FFFD */
    uint8_t bs[] = { 0xC0, 0x80 };
    feed_bytes(bs, 2);
    /* 0xC0 invalid lead → U+FFFD; 0x80 stray continuation → U+FFFD
     * Both buffered then flushed as a single 2-codepoint span. */
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(2, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32(0xFFFDu, g_events[0].as_print.cps[0]);
    TEST_ASSERT_EQUAL_UINT32(0xFFFDu, g_events[0].as_print.cps[1]);
}

void test_utf8_interrupted_by_esc(void)
{
    /* Start a 3-byte sequence, then send ESC [ A — parser should recover. */
    uint8_t lead[] = { 0xE2 };            /* start of ╔ */
    uint8_t csi[]  = { 0x1B, '[', 'A' }; /* ESC [ A = CUU 1 */
    feed_bytes(lead, 1);
    TEST_ASSERT_EQUAL_INT(0, g_event_count);
    feed_bytes(csi, 3);
    /* The incomplete UTF-8 emits U+FFFD (flushed before ESC), then CSI. */
    TEST_ASSERT_EQUAL_INT(2, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_PRINT, g_events[0].type);
    TEST_ASSERT_EQUAL_INT(1, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32(0xFFFDu, g_events[0].as_print.cps[0]);
    TEST_ASSERT_EQUAL_INT(VT_EV_CSI, g_events[1].type);
    TEST_ASSERT_EQUAL_UINT8('A', g_events[1].as_csi.final);
}

void test_utf8_mixed_ascii_and_multibyte(void)
{
    /* "A" é "B" — all buffered into one span */
    uint8_t bs[] = { 'A', 0xC3, 0xA9, 'B' };
    feed_bytes(bs, 4);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(3, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32('A',     g_events[0].as_print.cps[0]);
    TEST_ASSERT_EQUAL_UINT32(0x00E9u, g_events[0].as_print.cps[1]);
    TEST_ASSERT_EQUAL_UINT32('B',     g_events[0].as_print.cps[2]);
}

void test_utf8_state_persists_across_writes(void)
{
    /* Feed a 3-byte sequence byte-by-byte via separate vtparse_feed calls. */
    /* Я = U+042F → 0xD0 0xAF */
    uint8_t b0[] = { 0xD0 };
    uint8_t b1[] = { 0xAF };
    vtparse_feed(&g_parser, b0, 1);
    TEST_ASSERT_EQUAL_INT(0, g_event_count);
    vtparse_feed(&g_parser, b1, 1);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(1, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32(0x042Fu, g_events[0].as_print.cps[0]);
}

/* ════════════════════════════════════════════════════════════════════════════
 * State recovery
 * ══════════════════════════════════════════════════════════════════════════ */

void test_esc_interrupts_csi(void)
{
    /* Start a CSI, interrupt with ESC, then complete a new ESC sequence.
     * The partial CSI must be silently abandoned. */
    uint8_t bs[] = { 0x1B, '[', '1', '2',  /* partial CSI */
                     0x1B, 'M' };            /* ESC M = RI */
    feed_bytes(bs, 6);
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_ESC, g_events[0].type);
    TEST_ASSERT_EQUAL_UINT8('M', g_events[0].as_esc.final);
}

void test_0x18_resets_to_ground(void)
{
    /* 0x18 (CAN) anywhere → execute C0 + return to GROUND */
    uint8_t bs[] = { 0x1B, '[', '5',  /* partial CSI */
                     0x18,             /* CAN: cancel */
                     'A' };            /* printable in GROUND */
    feed_bytes(bs, 5);
    TEST_ASSERT_EQUAL_INT(2, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_C0,    g_events[0].type);
    TEST_ASSERT_EQUAL_UINT8(0x18,      g_events[0].as_c0.byte);
    TEST_ASSERT_EQUAL_INT(VT_EV_PRINT, g_events[1].type);
    TEST_ASSERT_EQUAL_INT(1, g_events[1].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32('A',      g_events[1].as_print.cps[0]);
}

void test_0x1a_resets_to_ground(void)
{
    /* 0x1A (SUB) anywhere → execute C0 + return to GROUND */
    uint8_t bs[] = { 0x1B, '[', 0x1A, 'B' };
    feed_bytes(bs, 4);
    TEST_ASSERT_EQUAL_INT(2, g_event_count);
    TEST_ASSERT_EQUAL_UINT8(0x1A, g_events[0].as_c0.byte);
    TEST_ASSERT_EQUAL_INT(1, g_events[1].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32('B', g_events[1].as_print.cps[0]);
}

void test_csi_ignore_returns_to_ground(void)
{
    /* Malformed CSI should not leave parser stuck.  Next sequence works. */
    uint8_t bs[] = { 0x1B, '[', '1', '?', 'h',  /* malformed → CSI_IGNORE */
                     0x1B, '[', '2', 'J' };       /* valid ED2 */
    feed_bytes(bs, sizeof(bs));
    TEST_ASSERT_EQUAL_INT(1, g_event_count);  /* only the valid one */
    TEST_ASSERT_EQUAL_UINT8('J', g_events[0].as_csi.final);
    TEST_ASSERT_EQUAL_INT32(2,   g_events[0].as_csi.params[0]);
}

void test_sos_pm_apc_ignored_until_st(void)
{
    /* SOS string content must not produce events; text after ST does. */
    uint8_t bs[] = { 0x1B, 'X',            /* SOS */
                     'j','u','n','k',       /* ignored */
                     0x1B, '\\',            /* ST */
                     'Z' };                 /* printable in GROUND */
    feed_bytes(bs, sizeof(bs));
    TEST_ASSERT_EQUAL_INT(1, g_event_count);
    TEST_ASSERT_EQUAL_INT(VT_EV_PRINT, g_events[0].type);
    TEST_ASSERT_EQUAL_INT(1, g_events[0].as_print.ncp);
    TEST_ASSERT_EQUAL_UINT32('Z', g_events[0].as_print.cps[0]);
}

/* ════════════════════════════════════════════════════════════════════════════
 * Test runner
 * ══════════════════════════════════════════════════════════════════════════ */

int main(void)
{
    UNITY_BEGIN();

    /* C0 controls */
    RUN_TEST(test_c0_bel);
    RUN_TEST(test_c0_bs);
    RUN_TEST(test_c0_lf_cr);
    RUN_TEST(test_c0_so_si);
    RUN_TEST(test_del_ignored);

    /* Printable ASCII */
    RUN_TEST(test_printable_ascii);

    /* ESC sequences */
    RUN_TEST(test_esc_D);
    RUN_TEST(test_esc_M);
    RUN_TEST(test_esc_7_8);
    RUN_TEST(test_esc_eq_gt);
    RUN_TEST(test_esc_with_intermediate);
    RUN_TEST(test_esc_charset_dec_special);

    /* CSI */
    RUN_TEST(test_csi_no_params);
    RUN_TEST(test_csi_one_param);
    RUN_TEST(test_csi_two_params);
    RUN_TEST(test_csi_default_first_param);
    RUN_TEST(test_csi_default_only);
    RUN_TEST(test_csi_private_question_mark);
    RUN_TEST(test_csi_private_gt);
    RUN_TEST(test_csi_intermediate_bang);
    RUN_TEST(test_csi_sub_params_colon);
    RUN_TEST(test_csi_truecolor_semicolons);
    RUN_TEST(test_csi_max_params);
    RUN_TEST(test_csi_malformed_second_private_marker);
    RUN_TEST(test_csi_multiple_sequences);

    /* OSC */
    RUN_TEST(test_osc_bel_terminated);
    RUN_TEST(test_osc_st_terminated);
    RUN_TEST(test_osc_null_terminated);
    RUN_TEST(test_osc_truncation);
    RUN_TEST(test_osc_8bit_st);

    /* DCS */
    RUN_TEST(test_dcs_hook_emitted);
    RUN_TEST(test_dcs_no_crash_on_data);

    /* UTF-8 */
    RUN_TEST(test_utf8_2byte);
    RUN_TEST(test_utf8_3byte);
    RUN_TEST(test_utf8_4byte_replaced_with_fffd);
    RUN_TEST(test_utf8_split_across_feeds);
    RUN_TEST(test_utf8_3byte_split_three_feeds);
    RUN_TEST(test_utf8_stray_continuation);
    RUN_TEST(test_utf8_invalid_overlong_c0);
    RUN_TEST(test_utf8_interrupted_by_esc);
    RUN_TEST(test_utf8_mixed_ascii_and_multibyte);
    RUN_TEST(test_utf8_state_persists_across_writes);

    /* State recovery */
    RUN_TEST(test_esc_interrupts_csi);
    RUN_TEST(test_0x18_resets_to_ground);
    RUN_TEST(test_0x1a_resets_to_ground);
    RUN_TEST(test_csi_ignore_returns_to_ground);
    RUN_TEST(test_sos_pm_apc_ignored_until_st);

    return UNITY_END();
}
