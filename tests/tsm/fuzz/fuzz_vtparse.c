#include "fuzz_common.h"

static void on_print(const uint32_t *cp, int n, void *user)
{
    CHECK(n > 0 && n <= VTP_PRINT_BUF);
    for (int i = 0; i < n; ++i) {
        record((trace_t *)user, 1);
        record((trace_t *)user, cp[i]);
    }
}
static void on_c0(uint8_t b, void *user)
{
    record((trace_t *)user, 2); record((trace_t *)user, b);
}
static void on_esc(uint8_t inter, uint8_t final, void *user)
{
    record((trace_t *)user, 3); record((trace_t *)user, inter); record((trace_t *)user, final);
}
static void sequence(uint32_t kind, uint8_t prefix, uint8_t inter, uint8_t final,
                     const int32_t *params, int n, void *user)
{
    CHECK(n >= 0 && n <= VTP_PARAMS_MAX);
    trace_t *t = (trace_t *)user;
    record(t, kind); record(t, prefix); record(t, inter); record(t, final); record(t, (uint32_t)n);
    for (int i = 0; i < n; ++i) {
        CHECK(params[i] >= -1);
        record(t, (uint32_t)params[i]);
    }
}
static void on_csi(uint8_t p, uint8_t i, uint8_t f, const int32_t *v, int n, void *u)
{ sequence(4, p, i, f, v, n, u); }
static void on_dcs(uint8_t p, uint8_t i, uint8_t f, const int32_t *v, int n, void *u)
{ sequence(5, p, i, f, v, n, u); }
static void on_osc(const uint8_t *data, int n, void *user)
{
    CHECK(n >= 0 && n <= VTP_OSC_MAX);
    CHECK(data[n] == 0);
    trace_t *t = (trace_t *)user;
    record(t, 6); record(t, (uint32_t)n);
    for (int i = 0; i < n; ++i) record(t, data[i]);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > FUZZ_MAX_INPUT) return 0;
    const vt_callbacks_t cb = {on_print, on_c0, on_esc, on_csi, on_osc, on_dcs};
    vtparse_t whole;
    trace_t expected = {0};
    vtparse_init(&whole, &cb, &expected);
    vtparse_feed(&whole, data, size);
    check_parser(&whole);
    for (int mode = 1; mode <= 2; ++mode) {
        vtparse_t split;
        trace_t actual = {0};
        vtparse_init(&split, &cb, &actual);
        for (size_t at = 0; at < size;) {
            size_t n = chunk_size(data, at, size, mode);
            vtparse_feed(&split, data + at, n);
            check_parser(&split);
            at += n;
        }
        compare_parser(&whole, &split);
        compare_trace(&expected, &actual);
        free(actual.words);
    }
    free(expected.words);
    return 0;
}
