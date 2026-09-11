#include "termstate.h"
#include "fuzz_common.h"

static void response(const char *data, size_t len, void *user)
{
    for (size_t i = 0; i < len; ++i) record((trace_t *)user, (uint8_t)data[i]);
}

static void check_terminal(const tsm_t *t)
{
    check_parser(&t->vtp);
    CHECK(t->cx >= 0 && t->cx < t->cols);
    CHECK(t->cy >= 0 && t->cy < t->rows);
    CHECK(t->base >= 0 && t->base < t->rows);
    CHECK(t->alt_base >= 0 && t->alt_base < t->rows);
    CHECK(t->scroll_top >= 0 && t->scroll_top <= t->scroll_bot);
    CHECK(t->scroll_bot < t->rows);
    CHECK(t->sb_off >= 0 && t->sb_off <= t->sb_len && t->sb_len <= t->sb_max);
    CHECK(t->sb_max == 0 || (t->sb_head >= 0 && t->sb_head < t->sb_max));
    for (int r = 0; r < t->rows; ++r) {
        const tsm_row_dirty_t d = tsm_dirty(t)[r];
        CHECK(d.l > d.r || d.r < t->cols);
        const tsm_cell_t *row = tsm_row(t, r);
        for (int c = 0; c < t->cols; ++c) {
            volatile uint16_t cp = row[c].cp;
            (void)cp;
        }
    }
}

static void compare_saved(const tsm_cursor_save_t *a, const tsm_cursor_save_t *b)
{
#define FIELD(f) CHECK(a->f == b->f)
    FIELD(col); FIELD(row); FIELD(attrs); FIELD(attrs2); FIELD(fg); FIELD(bg);
    FIELD(g0); FIELD(g1); FIELD(gl);
#undef FIELD
}

static void compare_terminal(const tsm_t *a, const tsm_t *b)
{
    check_terminal(a); check_terminal(b);
#define FIELD(f) CHECK(a->f == b->f)
    FIELD(cols); FIELD(rows); FIELD(base); FIELD(alt_base);
    FIELD(cx); FIELD(cy); FIELD(pending_wrap); FIELD(attrs); FIELD(attrs2); FIELD(fg); FIELD(bg);
    FIELD(g[0]); FIELD(g[1]); FIELD(gl); FIELD(scroll_top); FIELD(scroll_bot);
    FIELD(sb_max); FIELD(sb_len); FIELD(sb_head); FIELD(sb_off);
    FIELD(mode.lnm); FIELD(mode.irm); FIELD(mode.decom); FIELD(mode.decawm);
    FIELD(mode.dectcem); FIELD(mode.decalt); FIELD(mode.decckm); FIELD(mode.bracketed);
    FIELD(mode.mouse_x10); FIELD(mode.mouse_btn); FIELD(mode.sync_update);
#undef FIELD
    compare_saved(&a->saved, &b->saved);
    compare_saved(&a->alt_saved, &b->alt_saved);
    compare_parser(&a->vtp, &b->vtp);
    CHECK(strcmp(a->title, b->title) == 0);
    size_t bytes = (size_t)a->cols * (size_t)a->rows * sizeof(tsm_cell_t);
    CHECK(memcmp(a->cells, b->cells, bytes) == 0);
    CHECK(memcmp(a->alt_cells, b->alt_cells, bytes) == 0);
    if (a->sb_max) CHECK(memcmp(a->sb_cells, b->sb_cells,
        (size_t)a->sb_max * (size_t)a->cols * sizeof(tsm_cell_t)) == 0);
    for (int r = 0; r < a->rows; ++r) {
        CHECK(a->dirty[r].l == b->dirty[r].l && a->dirty[r].r == b->dirty[r].r);
        CHECK(memcmp(tsm_row(a, r), tsm_row(b, r), (size_t)a->cols * sizeof(tsm_cell_t)) == 0);
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > FUZZ_MAX_INPUT) return 0;
    static const int dimensions[][2] = {{1,1}, {1,4}, {7,1}, {7,3}, {80,24}, {220,60}};
    uint32_t hash = 0;
    for (size_t i = 0; i < size; ++i) hash = hash * 33u + data[i];
    unsigned dim = hash % 6u;
    int sb = (int)((hash / 6u) % 4u) * 3;
    tsm_t *a = tsm_new(dimensions[dim][0], dimensions[dim][1], sb);
    CHECK(a != NULL);
    trace_t expected = {0};
    tsm_set_response_cb(a, response, &expected);
    tsm_feed(a, data, size);
    check_terminal(a);
    for (int mode = 1; mode <= 2; ++mode) {
        tsm_t *b = tsm_new(a->cols, a->rows, sb);
        CHECK(b != NULL);
        trace_t actual = {0};
        tsm_set_response_cb(b, response, &actual);
        for (size_t at = 0; at < size;) {
            size_t n = chunk_size(data, at, size, mode);
            tsm_feed(b, data + at, n);
            /* Full row reads at checkpoints keep large screens affordable. */
            check_parser(&b->vtp);
            CHECK(b->cx >= 0 && b->cx < b->cols && b->cy >= 0 && b->cy < b->rows);
            at += n;
        }
        compare_terminal(a, b);
        compare_trace(&expected, &actual);
        tsm_sb_scroll(a, 100); tsm_sb_scroll(b, 100);
        compare_terminal(a, b);
        if (mode == 2) {
            tsm_clear_dirty(a); tsm_clear_dirty(b);
            tsm_feed(a, data, size);
            for (size_t at = 0; at < size;) {
                size_t n = chunk_size(data, at, size, mode);
                tsm_feed(b, data + at, n);
                at += n;
            }
            compare_terminal(a, b);
            compare_trace(&expected, &actual);
        }
        tsm_sb_scroll(a, -1); tsm_sb_scroll(b, -1);
        compare_terminal(a, b);
        tsm_sb_reset(a); tsm_sb_reset(b);
        tsm_free(b);
        free(actual.words);
    }
    tsm_reset(a);
    tsm_t *fresh = tsm_new(a->cols, a->rows, sb);
    CHECK(fresh != NULL);
    tsm_clear_dirty(a); tsm_clear_dirty(fresh);
    /* Reset need not erase inaccessible history storage or saved cursors. */
    check_terminal(a);
    for (int r = 0; r < a->rows; ++r)
        CHECK(memcmp(tsm_row(a, r), tsm_row(fresh, r), (size_t)a->cols * sizeof(tsm_cell_t)) == 0);
    CHECK(a->cx == fresh->cx && a->cy == fresh->cy && a->sb_len == 0 && a->sb_off == 0);
    tsm_free(fresh); tsm_free(a); free(expected.words);
    return 0;
}
