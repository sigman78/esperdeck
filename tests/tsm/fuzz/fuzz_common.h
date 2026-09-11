#pragma once
#include "vtparse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define FUZZ_MAX_INPUT 65536u

typedef struct {
    uint32_t *words;
    size_t len, cap;
} trace_t;

static void record(trace_t *t, uint32_t value)
{
    if (t->len == t->cap) {
        size_t cap = t->cap ? t->cap * 2 : 256;
        uint32_t *p = (uint32_t *)realloc(t->words, cap * sizeof(*p));
        CHECK(p != NULL);
        t->words = p;
        t->cap = cap;
    }
    t->words[t->len++] = value;
}

static void compare_trace(const trace_t *a, const trace_t *b)
{
    CHECK(a->len == b->len);
    if (a->len) CHECK(memcmp(a->words, b->words, a->len * sizeof(*a->words)) == 0);
}

static void check_parser(const vtparse_t *p)
{
    CHECK(p->state >= VTP_ST_GROUND && p->state <= VTP_ST_SOS_PM_APC);
    CHECK(p->prev_string <= VTP_STR_SOS);
    CHECK(p->utf8_remain >= 0 && p->utf8_remain <= 3);
    CHECK(p->nparams >= 0 && p->nparams <= VTP_PARAMS_MAX);
    CHECK(p->param_cur >= 0 && p->param_cur < VTP_PARAMS_MAX);
    CHECK(p->osc_len >= 0 && p->osc_len <= VTP_OSC_MAX);
    CHECK(p->print_len == 0);
    for (int i = 0; i < p->nparams; ++i) CHECK(p->params[i] >= -1);
}

static void compare_parser(const vtparse_t *a, const vtparse_t *b)
{
    check_parser(a);
    check_parser(b);
#define FIELD(f) CHECK(a->f == b->f)
    FIELD(state); FIELD(prev_string); FIELD(utf8_remain); FIELD(utf8_cp);
    FIELD(intermediate); FIELD(prefix); FIELD(nparams); FIELD(param_cur); FIELD(osc_len);
#undef FIELD
    CHECK(memcmp(a->params, b->params, (size_t)a->nparams * sizeof(a->params[0])) == 0);
    CHECK(memcmp(a->osc_buf, b->osc_buf, (size_t)a->osc_len) == 0);
}

static size_t chunk_size(const uint8_t *data, size_t at, size_t len, int mode)
{
    size_t n = mode == 1 ? 1 : 1 + (data[at] % 67u);
    return n < len - at ? n : len - at;
}
