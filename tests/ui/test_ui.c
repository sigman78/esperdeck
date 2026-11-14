/*
 * test_ui.c — the kit's layout and interaction logic at every grid the
 * deck ships (100x30, 80x24, 66x20). Guards the contracts screens lean
 * on. Cuts never overlap or leak. Grids stay inside their area. Touch
 * targets keep an odd height. Arrow keys reach every region.
 */

#include "unity.h"
#include <string.h>
#include "ui_hot.h"
#include "ui_layout.h"
#include "ui_part.h"
#include "ui_pure.h"

static const struct { int cols, rows; } GRIDS[] = {
    { 100, 30 }, { 80, 24 }, { 66, 20 },
};
#define NGRIDS ((int)(sizeof GRIDS / sizeof *GRIDS))

void setUp(void) { ui_hot_reset(); ui_hot_begin(0); }
void tearDown(void) {}

static void test_cuts_partition_the_rect(void)
{
    ui_rect_t r = ui_rect(0, 0, 66, 20);
    ui_rect_t head = ui_cut_top(&r, 3);
    ui_rect_t bar  = ui_cut_bottom(&r, 1);
    TEST_ASSERT_EQUAL(0,  head.y);
    TEST_ASSERT_EQUAL(3,  head.h);
    TEST_ASSERT_EQUAL(19, bar.y);
    TEST_ASSERT_EQUAL(1,  bar.h);
    TEST_ASSERT_EQUAL(3,  r.y);
    TEST_ASSERT_EQUAL(16, r.h);

    ui_rect_t left  = ui_cut_left(&r, 10);
    ui_rect_t right = ui_cut_right(&r, 6);
    TEST_ASSERT_EQUAL(0,  left.x);
    TEST_ASSERT_EQUAL(60, right.x);
    TEST_ASSERT_EQUAL(10, r.x);
    TEST_ASSERT_EQUAL(50, r.w);
}

static void test_oversized_cut_takes_all_and_no_more(void)
{
    ui_rect_t r = ui_rect(2, 2, 5, 4);
    ui_rect_t s = ui_cut_top(&r, 99);
    TEST_ASSERT_EQUAL(4, s.h);
    TEST_ASSERT_TRUE(ui_rect_empty(r));
    s = ui_cut_left(&r, -3);
    TEST_ASSERT_EQUAL(0, s.w);
}

static void test_center_and_inset_stay_inside(void)
{
    ui_rect_t o = ui_rect(4, 3, 20, 9);
    ui_rect_t c = ui_center(o, 10, 3);
    TEST_ASSERT_EQUAL(9, c.x);
    TEST_ASSERT_EQUAL(6, c.y);
    c = ui_center(o, 99, 99);
    TEST_ASSERT_EQUAL(20, c.w);
    TEST_ASSERT_EQUAL(9,  c.h);
    TEST_ASSERT_TRUE(ui_rect_empty(ui_inset(o, 10, 0)));
}

static void test_metrics_keep_touch_heights_odd(void)
{
    for (int i = 0; i < NGRIDS; i++) {
        ui_metrics_t m = ui_metrics_for(GRIDS[i].cols, GRIDS[i].rows);
        TEST_ASSERT_EQUAL(1, m.unit % 2);
        TEST_ASSERT_EQUAL(1, m.header % 2);
        TEST_ASSERT_TRUE(m.unit >= 3);
    }
    TEST_ASSERT_EQUAL(UI_WIDE,    ui_metrics_for(100, 30).cls);
    TEST_ASSERT_EQUAL(UI_REGULAR, ui_metrics_for(80, 24).cls);
    TEST_ASSERT_EQUAL(UI_COMPACT, ui_metrics_for(66, 20).cls);
}

/* The body under a header and above the status row. */
static ui_rect_t body_of(int cols, int rows)
{
    ui_metrics_t m = ui_metrics_for(cols, rows);
    ui_rect_t r = ui_rect(0, 0, cols, rows);
    ui_cut_top(&r, m.header);
    ui_cut_bottom(&r, 1);
    return ui_inset(r, m.margin, 0);
}

static void test_grid_cells_tile_without_overlap(void)
{
    for (int i = 0; i < NGRIDS; i++) {
        ui_rect_t body = body_of(GRIDS[i].cols, GRIDS[i].rows);
        ui_grid_t g = ui_grid(body, 2, 3, 2, 0);
        TEST_ASSERT_TRUE(ui_grid_capacity(&g) > 0);
        for (int a = 0; a < ui_grid_capacity(&g); a++) {
            ui_rect_t ra = ui_grid_cell(&g, a);
            TEST_ASSERT_TRUE(ra.x >= body.x && ra.x + ra.w <= body.x + body.w);
            TEST_ASSERT_TRUE(ra.y >= body.y && ra.y + ra.h <= body.y + body.h);
            for (int b = a + 1; b < ui_grid_capacity(&g); b++) {
                ui_rect_t rb = ui_grid_cell(&g, b);
                const bool apart = ra.x + ra.w <= rb.x || rb.x + rb.w <= ra.x ||
                                   ra.y + ra.h <= rb.y || rb.y + rb.h <= ra.y;
                TEST_ASSERT_TRUE(apart);
            }
        }
        TEST_ASSERT_TRUE(ui_rect_empty(ui_grid_cell(&g, ui_grid_capacity(&g))));
    }
}

/* The setup hub holds its eight sections as 4-row tiles at every grid.
 * The columns come from ui_cols_for, so no tile is narrower than 24. */
static void test_hub_tiles_fit_at_every_grid(void)
{
    for (int i = 0; i < NGRIDS; i++) {
        const ui_metrics_t m = ui_metrics_for(GRIDS[i].cols, GRIDS[i].rows);
        ui_rect_t body = body_of(GRIDS[i].cols, GRIDS[i].rows);
        const int cols = ui_cols_for(body.w, 24, m.gutter);
        TEST_ASSERT_TRUE(cols >= 2);
        ui_grid_t g = ui_grid(body, cols, 4, m.gutter, 0);
        TEST_ASSERT_TRUE(ui_grid_capacity(&g) >= 8);
        TEST_ASSERT_TRUE(ui_grid_cell(&g, 0).w >= 24);
    }
}

/* A section lays one choice or one pack of parts per 3-row line. More
 * lines than the body holds make pages. Every line lands on one page. */
static void test_section_pages_instead_of_overflowing(void)
{
    for (int i = 0; i < NGRIDS; i++) {
        const ui_rect_t body = body_of(GRIDS[i].cols, GRIDS[i].rows);
        const int per = body.h / 3;
        TEST_ASSERT_TRUE(per >= 1);
        TEST_ASSERT_EQUAL(1, ui_page_count(per, per));
        TEST_ASSERT_EQUAL(2, ui_page_count(per + 1, per));
        for (int lines = 1; lines <= 12; lines++) {
            const int pages = ui_page_count(lines, per);
            TEST_ASSERT_TRUE(pages * per >= lines);
            TEST_ASSERT_TRUE((pages - 1) * per < lines);
            TEST_ASSERT_EQUAL(pages - 1, ui_page_clamp(99, lines, per));
        }
    }
}

/* A 2x3 grid of regions, ids 1..6 row-major, plus the back target. */
static void lay_six(void)
{
    ui_hot_begin(0);
    ui_hot(ui_rect(0, 0, 10, 3), UI_ID_BACK);
    ui_grid_t g = ui_grid(ui_rect(1, 3, 64, 9), 2, 3, 2, 0);
    for (int i = 0; i < 6; i++)
        ui_hot(ui_grid_cell(&g, i), (ui_id_t)(i + 1));
}

/* The storage limit is 8 profiles. A two-column picker holds them all
 * at every grid. HOME above a 3-row action row holds 6 two-line cards
 * at 66x20. That is the one place paging can occur. */
static void test_profile_limit_fits_or_pages(void)
{
    static const int GRIDS[3][2] = { { 100, 30 }, { 80, 24 }, { 66, 20 } };
    for (int i = 0; i < 3; i++) {
        const ui_metrics_t m = ui_metrics_for(GRIDS[i][0], GRIDS[i][1]);
        ui_rect_t body = ui_rect(m.margin, m.header,
                                 GRIDS[i][0] - 2 * m.margin,
                                 GRIDS[i][1] - m.header - 1);
        ui_grid_t pick = ui_grid(body, 2, 4, m.gutter, 0);
        TEST_ASSERT_TRUE(ui_grid_capacity(&pick) >= 8);

        ui_cut_bottom(&body, m.unit);
        ui_grid_t one = ui_grid(body, 2, m.unit, m.gutter, 0);
        TEST_ASSERT_TRUE(ui_grid_capacity(&one) >= 8);
    }
    ui_rect_t body = ui_rect(1, 3, 64, 16);
    ui_cut_bottom(&body, 3);
    ui_grid_t two = ui_grid(body, 2, 4, 2, 0);
    TEST_ASSERT_EQUAL(6, ui_grid_capacity(&two));
    TEST_ASSERT_EQUAL(2, ui_page_count(8, 6));
}

static void test_paging_counts_and_clamps(void)
{
    TEST_ASSERT_EQUAL(1, ui_page_count(0, 6));
    TEST_ASSERT_EQUAL(1, ui_page_count(6, 6));
    TEST_ASSERT_EQUAL(2, ui_page_count(7, 6));
    TEST_ASSERT_EQUAL(3, ui_page_count(13, 6));
    TEST_ASSERT_EQUAL(1, ui_page_count(9, 0));
    TEST_ASSERT_EQUAL(0, ui_page_clamp(-1, 8, 6));
    TEST_ASSERT_EQUAL(1, ui_page_clamp(5, 8, 6));
    TEST_ASSERT_EQUAL(0, ui_page_clamp(1, 6, 6));
}

static void test_hit_resolves_a_cell_to_its_region(void)
{
    lay_six();
    TEST_ASSERT_EQUAL(UI_ID_BACK, ui_hot_at(2, 1));
    TEST_ASSERT_EQUAL(1, ui_hot_at(1, 3));
    TEST_ASSERT_EQUAL(2, ui_hot_at(40, 4));
    TEST_ASSERT_EQUAL(5, ui_hot_at(5, 10));
    TEST_ASSERT_EQUAL(0, ui_hot_at(32, 4));     /* the column gutter */
    TEST_ASSERT_EQUAL(0, ui_hot_at(5, 15));     /* below the grid    */
}

static void test_default_focus_skips_the_back_target(void)
{
    lay_six();
    TEST_ASSERT_EQUAL(1, ui_focus_get());
    TEST_ASSERT_TRUE(ui_focused(1));
}

static void test_arrows_walk_the_grid(void)
{
    lay_six();
    TEST_ASSERT_EQUAL(2, ui_hot_key(K_RIGHT).id);
    TEST_ASSERT_EQUAL(4, ui_hot_key(K_DOWN).id);
    TEST_ASSERT_EQUAL(3, ui_hot_key(K_LEFT).id);
    TEST_ASSERT_EQUAL(5, ui_hot_key(K_DOWN).id);
    TEST_ASSERT_EQUAL(UI_ACT_EDGE, ui_hot_key(K_DOWN).kind);
    TEST_ASSERT_EQUAL(UI_ACT_EDGE, ui_hot_key(K_LEFT).kind);
    TEST_ASSERT_EQUAL(5, ui_focus_get());
}

static void test_up_from_the_top_row_reaches_back(void)
{
    lay_six();
    TEST_ASSERT_EQUAL(UI_ID_BACK, ui_hot_key(K_UP).id);
    TEST_ASSERT_EQUAL(1, ui_hot_key(K_DOWN).id);
}

static void test_every_region_is_reachable_by_arrows(void)
{
    lay_six();
    bool seen[7] = { false };
    static const ui_key_t WALK[] = {
        K_RIGHT, K_DOWN, K_LEFT, K_DOWN, K_RIGHT, K_UP, K_UP, K_LEFT,
    };
    seen[ui_focus_get()] = true;
    for (unsigned i = 0; i < sizeof WALK / sizeof *WALK; i++) {
        ui_act_t a = ui_hot_key(WALK[i]);
        if (a.id && a.id != UI_ID_BACK) seen[a.id] = true;
    }
    for (int id = 1; id <= 6; id++) TEST_ASSERT_TRUE(seen[id]);
}

static void test_enter_presses_the_focused_region(void)
{
    lay_six();
    ui_hot_key(K_RIGHT);
    ui_act_t a = ui_hot_key(K_ENTER);
    TEST_ASSERT_EQUAL(UI_ACT_PRESS, a.kind);
    TEST_ASSERT_EQUAL(2, a.id);
}

static void test_tap_presses_and_hides_the_ring(void)
{
    lay_six();
    ui_act_t a = ui_hot_touch(UI_TOUCH_TAP, 40, 7);
    TEST_ASSERT_EQUAL(UI_ACT_PRESS, a.kind);
    TEST_ASSERT_EQUAL(4, a.id);
    TEST_ASSERT_FALSE(ui_focused(4));
    TEST_ASSERT_TRUE(ui_pressed(4));

    /* The next key only reveals the ring, on the tapped region. */
    a = ui_hot_key(K_DOWN);
    TEST_ASSERT_EQUAL(UI_ACT_MOVE, a.kind);
    TEST_ASSERT_EQUAL(4, a.id);
    TEST_ASSERT_TRUE(ui_focused(4));
    TEST_ASSERT_EQUAL(6, ui_hot_key(K_DOWN).id);
}

static void test_tap_on_a_gutter_does_nothing(void)
{
    lay_six();
    ui_hot_key(K_RIGHT);
    ui_hot_touch(UI_TOUCH_TAP, 40, 7);
    ui_act_t a = ui_hot_touch(UI_TOUCH_TAP, 32, 7);
    TEST_ASSERT_EQUAL(UI_ACT_NONE, a.kind);
    TEST_ASSERT_EQUAL(0, a.id);
}

static void test_pressed_follows_the_finger_and_the_flash_expires(void)
{
    lay_six();
    ui_hot_touch(UI_TOUCH_DOWN, 5, 4);
    TEST_ASSERT_TRUE(ui_pressed(1));
    ui_hot_touch(UI_TOUCH_UP, 5, 4);
    TEST_ASSERT_FALSE(ui_pressed(1));

    ui_hot_touch(UI_TOUCH_TAP, 5, 4);
    TEST_ASSERT_TRUE(ui_pressed(1));
    TEST_ASSERT_TRUE(ui_hot_animating());
    ui_hot_begin(UI_FLASH_FRAMES);
    TEST_ASSERT_FALSE(ui_pressed(1));
}

static void test_reset_drops_regions_and_focus(void)
{
    lay_six();
    ui_hot_reset();
    TEST_ASSERT_EQUAL(0, ui_hot_at(5, 4));
    TEST_ASSERT_EQUAL(0, ui_focus_get());
    TEST_ASSERT_EQUAL(UI_ACT_NONE, ui_hot_key(K_DOWN).kind);
}

static void test_region_table_overflow_is_dropped_not_written(void)
{
    ui_hot_begin(0);
    for (int i = 0; i < UI_HOT_MAX + 8; i++)
        ui_hot(ui_rect(i % 60, i / 60, 1, 1), (ui_id_t)(i + 1));
    TEST_ASSERT_EQUAL(UI_HOT_MAX, ui_hot_at((UI_HOT_MAX - 1) % 60,
                                            (UI_HOT_MAX - 1) / 60));
    TEST_ASSERT_EQUAL(0, ui_hot_at(UI_HOT_MAX % 60, UI_HOT_MAX / 60));
}

static void test_len_counts_code_points(void)
{
    TEST_ASSERT_EQUAL(0, ui_len(NULL));
    TEST_ASSERT_EQUAL(0, ui_len(""));
    TEST_ASSERT_EQUAL(3, ui_len("abc"));
    TEST_ASSERT_EQUAL(8, ui_len(UI_S_CHECK " joined"));
    TEST_ASSERT_EQUAL(1, ui_len(UI_S_CHEVRON));
    TEST_ASSERT_EQUAL(2, ui_len(UI_S_MIDDOT "x"));
    TEST_ASSERT_EQUAL(1, ui_len("\xE2"));                /* cut sequence    */
    TEST_ASSERT_EQUAL(1, ui_len("\xF0\x9F\x98\x80"));    /* beyond the BMP  */
}

static void test_one_decoder_walks_a_string(void)
{
    const char *p = UI_S_MIDDOT "a" UI_S_CHECK;
    TEST_ASSERT_EQUAL_HEX16(0x00B7, ui_utf8_next(&p));
    TEST_ASSERT_EQUAL_HEX16('a',    ui_utf8_next(&p));
    TEST_ASSERT_EQUAL_HEX16(0x2713, ui_utf8_next(&p));
    TEST_ASSERT_EQUAL_HEX16(0,      ui_utf8_next(&p));
    p = "\xF0\x9F\x98\x80z";
    TEST_ASSERT_EQUAL_HEX16('?', ui_utf8_next(&p));
    TEST_ASSERT_EQUAL_HEX16('z', ui_utf8_next(&p));
}

static void test_put_writes_what_the_decoder_reads(void)
{
    static const uint16_t CPS[] = { 'A', 0x7F, 0x80, 0xB7, 0x7FF, 0x800,
                                    0x2713, 0xFFFD };
    for (size_t i = 0; i < sizeof CPS / sizeof CPS[0]; i++) {
        char buf[5];
        const int n = ui_utf8_put(buf, CPS[i]);
        const char *p = buf;
        TEST_ASSERT_EQUAL(CPS[i] < 0x80 ? 1 : CPS[i] < 0x800 ? 2 : 3, n);
        TEST_ASSERT_EQUAL(0, buf[n]);
        TEST_ASSERT_EQUAL_UINT16(CPS[i], ui_utf8_next(&p));
        TEST_ASSERT_EQUAL_PTR(buf + n, p);
    }
}

static void test_fit_counts_cells_not_bytes(void)
{
    bool cut = true;
    TEST_ASSERT_EQUAL(3, ui_fit("abc", 8, &cut));
    TEST_ASSERT_FALSE(cut);
    TEST_ASSERT_EQUAL(8, ui_fit("abcdefghij", 8, &cut));
    TEST_ASSERT_TRUE(cut);
    TEST_ASSERT_EQUAL(4, ui_fit(UI_S_CHECK " ok", 4, &cut));
    TEST_ASSERT_FALSE(cut);                          /* 4 cells, 6 bytes */
    TEST_ASSERT_EQUAL(3, ui_fit(UI_S_CHECK " ok", 3, &cut));
    TEST_ASSERT_TRUE(cut);
    TEST_ASSERT_EQUAL(0, ui_fit(NULL, 3, &cut));
}

static bool num(const char *w) { return ui_is_number(w, (int)strlen(w)); }

static void test_number_words(void)
{
    TEST_ASSERT_TRUE(num("55"));
    TEST_ASSERT_TRUE(num("-55"));
    TEST_ASSERT_TRUE(num("+7"));
    TEST_ASSERT_TRUE(num("2.5"));
    TEST_ASSERT_TRUE(num("14:07"));
    TEST_ASSERT_FALSE(num(""));
    TEST_ASSERT_FALSE(num("-"));
    TEST_ASSERT_FALSE(num("2."));
    TEST_ASSERT_FALSE(num(".5"));
    TEST_ASSERT_FALSE(num("1.2.3"));
    TEST_ASSERT_FALSE(num("192.168.1.10"));
    TEST_ASSERT_FALSE(num("12:30:45"));
    TEST_ASSERT_FALSE(num("dBm"));
    TEST_ASSERT_FALSE(num("5x"));
    TEST_ASSERT_FALSE(num("--5"));
    TEST_ASSERT_TRUE(ui_is_number("12abc", 2));      /* the length rules */
}

static void test_centre_counts_an_uneven_split(void)
{
    const int before = ui_faults();
    TEST_ASSERT_EQUAL(6, ui_centre(5, 3, 1));
    TEST_ASSERT_EQUAL(6, ui_centre(5, 4, 2));
    TEST_ASSERT_EQUAL(7, ui_centre(4, 6, 0));
    TEST_ASSERT_EQUAL(before, ui_faults());
    TEST_ASSERT_EQUAL(6, ui_centre(5, 4, 1));        /* 3 spare rows */
    TEST_ASSERT_EQUAL(before + 1, ui_faults());
    ui_fault();
    TEST_ASSERT_EQUAL(before + 2, ui_faults());
}

static void test_rail_thumb_rounds_half_up(void)
{
    int at = -1;
    TEST_ASSERT_EQUAL(6, ui_rail_thumb(12, 0, 2, &at));
    TEST_ASSERT_EQUAL(0, at);
    TEST_ASSERT_EQUAL(6, ui_rail_thumb(12, 1, 2, &at));
    TEST_ASSERT_EQUAL(6, at);
    TEST_ASSERT_EQUAL(4, ui_rail_thumb(12, 1, 3, &at));
    TEST_ASSERT_EQUAL(4, at);                        /* 8 * 1 / 2        */
    /* 5 rows, 2 pages: thumb 2.5 rounds to 3. */
    TEST_ASSERT_EQUAL(3, ui_rail_thumb(5, 0, 2, &at));
    TEST_ASSERT_EQUAL(0, at);
    TEST_ASSERT_EQUAL(3, ui_rail_thumb(5, 1, 2, &at));
    TEST_ASSERT_EQUAL(2, at);
    /* 3 rows, 4 pages: thumb 0.75 rounds to 1; 2 * 1 / 3 rounds to 1. */
    TEST_ASSERT_EQUAL(1, ui_rail_thumb(3, 1, 4, &at));
    TEST_ASSERT_EQUAL(1, at);
    /* 2 rows, 5 pages: thumb 0.4 stays at the minimum of 1. */
    TEST_ASSERT_EQUAL(1, ui_rail_thumb(2, 4, 5, &at));
    TEST_ASSERT_EQUAL(1, at);
    /* 10 rows, 4 pages: thumb 2.5 -> 3; 7 * 1 / 3 = 2.33 -> 2. */
    TEST_ASSERT_EQUAL(3, ui_rail_thumb(10, 1, 4, &at));
    TEST_ASSERT_EQUAL(2, at);
    /* A single page fills the track. */
    TEST_ASSERT_EQUAL(9, ui_rail_thumb(9, 0, 1, &at));
    TEST_ASSERT_EQUAL(0, at);
}

static void test_hold_key_holds_the_focused_region(void)
{
    lay_six();
    ui_hot_touch(UI_TOUCH_TAP, 5, 4);               /* hides the ring    */
    ui_act_t a = ui_hot_key(K_HOLD);
    TEST_ASSERT_EQUAL(UI_ACT_MOVE, a.kind);         /* only the ring     */
    TEST_ASSERT_EQUAL(1, a.id);
    TEST_ASSERT_TRUE(ui_focused(1));
    ui_hot_key(K_RIGHT);
    a = ui_hot_key(K_HOLD);
    TEST_ASSERT_EQUAL(UI_ACT_HOLD, a.kind);
    TEST_ASSERT_EQUAL(2, a.id);
    TEST_ASSERT_FALSE(ui_pressed(2));               /* a hold is no tap  */
}

static void test_release_after_a_hold_presses_nothing(void)
{
    lay_six();
    ui_act_t a = ui_hot_touch(UI_TOUCH_DOWN, 5, 4);
    TEST_ASSERT_EQUAL(1, a.id);
    a = ui_hot_touch(UI_TOUCH_HOLD, 5, 4);
    TEST_ASSERT_EQUAL(UI_ACT_HOLD, a.kind);
    TEST_ASSERT_EQUAL(1, a.id);
    a = ui_hot_touch(UI_TOUCH_UP, 5, 4);
    TEST_ASSERT_NOT_EQUAL(UI_ACT_PRESS, a.kind);
    TEST_ASSERT_FALSE(ui_pressed(1));
    a = ui_hot_touch(UI_TOUCH_TAP, 5, 7);
    TEST_ASSERT_EQUAL(UI_ACT_PRESS, a.kind);
    TEST_ASSERT_EQUAL(3, a.id);
}

static void test_focus_default_only_fills_an_empty_focus(void)
{
    ui_hot_reset();
    ui_hot_begin(0);
    ui_focus_default(4);
    ui_hot(ui_rect(0, 0, 4, 3), 1);
    ui_hot(ui_rect(5, 0, 4, 3), 4);
    TEST_ASSERT_EQUAL(4, ui_focus_get());
    ui_focus_default(1);                            /* focus already set */
    TEST_ASSERT_EQUAL(4, ui_focus_get());

    ui_hot_reset();
    ui_hot_begin(0);
    ui_hot(ui_rect(0, 0, 4, 3), 1);                 /* first region wins */
    ui_focus_default(4);
    TEST_ASSERT_EQUAL(1, ui_focus_get());
}

static void test_debug_pose_forces_a_look(void)
{
    lay_six();
    ui_hot_touch(UI_TOUCH_UP, 0, 0);                /* ring hidden       */
    TEST_ASSERT_FALSE(ui_focused(3));
    ui_debug_pose(3, UI_POSE_FOCUS);
    ui_debug_pose(4, UI_POSE_DOWN);
    TEST_ASSERT_TRUE(ui_focused(3));
    TEST_ASSERT_FALSE(ui_pressed(3));
    TEST_ASSERT_TRUE(ui_pressed(4));
    TEST_ASSERT_FALSE(ui_focused(4));
    TEST_ASSERT_FALSE(ui_focused(5));
    ui_debug_pose(3, UI_POSE_DOWN);                 /* a new pose replaces */
    TEST_ASSERT_FALSE(ui_focused(3));
    TEST_ASSERT_TRUE(ui_pressed(3));
    for (int i = 0; i < 16; i++)
        ui_debug_pose((ui_id_t)(100 + i), UI_POSE_FOCUS);
    ui_debug_pose(200, UI_POSE_FOCUS);              /* table full: dropped */
    TEST_ASSERT_FALSE(ui_focused(200));
    ui_hot_reset();
    TEST_ASSERT_FALSE(ui_pressed(3));
    TEST_ASSERT_FALSE(ui_focused(100));
}

/* A flow packs parts at their own width. A part with no room starts the
 * next row. A part wider than the flow is cut to it. */
static void test_flow_wraps_breaks_and_cuts(void)
{
    ui_flow_t f = ui_flow(ui_rect(2, 5, 30, 4), 2, 3);
    ui_rect_t a = ui_flow_next(&f, 10);
    ui_rect_t b = ui_flow_next(&f, 10);
    ui_rect_t c = ui_flow_next(&f, 10);             /* 12+12+10 > 30     */
    TEST_ASSERT_EQUAL(2,  a.x);
    TEST_ASSERT_EQUAL(5,  a.y);
    TEST_ASSERT_EQUAL(3,  a.h);
    TEST_ASSERT_EQUAL(14, b.x);
    TEST_ASSERT_EQUAL(5,  b.y);
    TEST_ASSERT_EQUAL(2,  c.x);
    TEST_ASSERT_EQUAL(8,  c.y);

    ui_flow_break(&f);                              /* row has a part    */
    TEST_ASSERT_EQUAL(11, f.y);
    ui_flow_break(&f);                              /* row is empty      */
    TEST_ASSERT_EQUAL(11, f.y);
    ui_rect_t d = ui_flow_next(&f, 50);             /* wider than the flow */
    TEST_ASSERT_EQUAL(2,  d.x);
    TEST_ASSERT_EQUAL(30, d.w);
    TEST_ASSERT_EQUAL(11, d.y);
    ui_rect_t e = ui_flow_next(&f, 4);
    TEST_ASSERT_EQUAL(2,  e.x);
    TEST_ASSERT_EQUAL(14, e.y);
}

static void test_own_widths(void)
{
    TEST_ASSERT_EQUAL(10, ui_push_w("Edit"));
    TEST_ASSERT_EQUAL(12, ui_push_w("Cancel"));
    TEST_ASSERT_EQUAL(20, ui_push_w(UI_S_UP " Move earlier"));
    TEST_ASSERT_EQUAL(10, ui_act_w("Key", NULL));
    TEST_ASSERT_EQUAL(25, ui_act_w("Key", "deck-ed25519 " UI_S_CHEVRON));
    TEST_ASSERT_EQUAL(25, ui_act_w("Add network (phone)", ""));
    TEST_ASSERT_EQUAL(30, ui_switch_w("Show password"));
    TEST_ASSERT_EQUAL(8,  ui_caption_w("Tint"));
}

static void test_cols_grow_with_the_grid(void)
{
    TEST_ASSERT_EQUAL(2, ui_cols_for(30, 30, 2));   /* two at least      */
    TEST_ASSERT_EQUAL(2, ui_cols_for(64, 30, 2));
    TEST_ASSERT_EQUAL(3, ui_cols_for(94, 30, 2));
    TEST_ASSERT_EQUAL(4, ui_cols_for(126, 30, 2));
    TEST_ASSERT_EQUAL(3, ui_cols_for(78, 24, 2));
}

/* The next row straight ahead wins over an aligned region two rows away.
 * Its left edge may be far off. */
static void test_arrow_prefers_the_next_row_straight_ahead(void)
{
    ui_hot_reset();
    ui_hot_begin(0);
    ui_hot(ui_rect(20, 0, 10, 3), 1);
    ui_hot(ui_rect(0, 3, 24, 3), 2);                /* wide, edge far off */
    ui_hot(ui_rect(20, 6, 10, 3), 3);               /* aligned, 2 rows   */
    ui_hot_key(K_HOLD);                             /* shows the ring    */
    TEST_ASSERT_EQUAL(2, ui_hot_key(K_DOWN).id);
}

/* A region straight ahead beats a nearer one off to the side. */
static void test_arrow_prefers_straight_ahead_to_the_side(void)
{
    ui_hot_reset();
    ui_hot_begin(0);
    ui_hot(ui_rect(20, 0, 10, 3), 1);
    ui_hot(ui_rect(0, 3, 10, 3), 2);                /* nearer, aside     */
    ui_hot(ui_rect(20, 9, 10, 3), 3);               /* far, ahead        */
    ui_hot_key(K_HOLD);                             /* shows the ring    */
    TEST_ASSERT_EQUAL(3, ui_hot_key(K_DOWN).id);
}

static void test_entry_window_keeps_the_caret_in_view(void)
{
    /* Room 10 shows nine characters and the caret cell. */
    TEST_ASSERT_EQUAL(0, ui_entry_first(0, 0, 10));
    TEST_ASSERT_EQUAL(0, ui_entry_first(9, 9, 10));      /* fits         */
    TEST_ASSERT_EQUAL(1, ui_entry_first(10, 10, 10));    /* tail         */
    TEST_ASSERT_EQUAL(11, ui_entry_first(20, 20, 10));   /* tail         */
    TEST_ASSERT_EQUAL(0, ui_entry_first(20, 0, 10));     /* head         */
    TEST_ASSERT_EQUAL(0, ui_entry_first(20, 5, 10));
    TEST_ASSERT_EQUAL(5, ui_entry_first(20, 10, 10));    /* middle       */
    for (int len = 0; len <= 40; len++)
        for (int caret = 0; caret <= len; caret++)
            for (int room = 3; room <= 12; room++) {
                const int f = ui_entry_first(len, caret, room);
                TEST_ASSERT_TRUE(f >= 0);
                TEST_ASSERT_TRUE(caret >= f);
                TEST_ASSERT_TRUE(caret - f <= room - 1);
                if (len <= room - 1) TEST_ASSERT_EQUAL(0, f);
                if (caret == len && len > room - 1)
                    TEST_ASSERT_EQUAL(len - (room - 1), f);
            }
}

static void test_text_insert_respects_caret_and_max(void)
{
    char b[8] = "ac";
    int c = 1;
    TEST_ASSERT_TRUE(ui_text_insert(b, 5, &c, 'b'));
    TEST_ASSERT_EQUAL_STRING("abc", b);
    TEST_ASSERT_EQUAL(2, c);
    TEST_ASSERT_TRUE(ui_text_insert(b, 5, &c, 'X'));
    TEST_ASSERT_EQUAL_STRING("abXc", b);
    TEST_ASSERT_TRUE(ui_text_insert(b, 5, &c, 'Y'));
    TEST_ASSERT_EQUAL_STRING("abXYc", b);
    TEST_ASSERT_FALSE(ui_text_insert(b, 5, &c, 'Z'));    /* full        */
    TEST_ASSERT_EQUAL_STRING("abXYc", b);
    TEST_ASSERT_EQUAL(4, c);
    c = 0;
    TEST_ASSERT_FALSE(ui_text_insert(b, 5, &c, '\n'));   /* control     */
    TEST_ASSERT_EQUAL_STRING("abXYc", b);
}

static void test_text_delete_takes_the_char_before_the_caret(void)
{
    char b[8] = "abcd";
    int c = 0;
    TEST_ASSERT_FALSE(ui_text_delete(b, &c));
    TEST_ASSERT_EQUAL_STRING("abcd", b);
    c = 2;
    TEST_ASSERT_TRUE(ui_text_delete(b, &c));
    TEST_ASSERT_EQUAL_STRING("acd", b);
    TEST_ASSERT_EQUAL(1, c);
    c = 3;
    TEST_ASSERT_TRUE(ui_text_delete(b, &c));
    TEST_ASSERT_EQUAL_STRING("ac", b);
    TEST_ASSERT_EQUAL(2, c);
    c = 9;                                   /* past the end: clamped   */
    TEST_ASSERT_TRUE(ui_text_delete(b, &c));
    TEST_ASSERT_EQUAL_STRING("a", b);
    TEST_ASSERT_EQUAL(1, c);
}

static void test_code_plan_spaces_then_packs_then_clips(void)
{
    ui_code_t c = ui_code_plan(0, 0, 25);
    TEST_ASSERT_EQUAL(0, c.bullets + c.dots);
    TEST_ASSERT_EQUAL(0, c.width);
    c = ui_code_plan(3, 4, 25);                  /* three of four        */
    TEST_ASSERT_TRUE(c.spaced);
    TEST_ASSERT_EQUAL(3, c.bullets);
    TEST_ASSERT_EQUAL(1, c.dots);
    TEST_ASSERT_EQUAL(7, c.width);
    c = ui_code_plan(5, 0, 25);                  /* free length          */
    TEST_ASSERT_EQUAL(5, c.bullets);
    TEST_ASSERT_EQUAL(0, c.dots);
    TEST_ASSERT_EQUAL(9, c.width);
    c = ui_code_plan(13, 0, 25);                 /* 25 spaced cells fit  */
    TEST_ASSERT_TRUE(c.spaced);
    TEST_ASSERT_EQUAL(25, c.width);
    c = ui_code_plan(14, 0, 25);                 /* drop the spaces      */
    TEST_ASSERT_FALSE(c.spaced);
    TEST_ASSERT_EQUAL(14, c.width);
    c = ui_code_plan(40, 0, 25);                 /* last bullets only    */
    TEST_ASSERT_FALSE(c.spaced);
    TEST_ASSERT_EQUAL(25, c.bullets);
    TEST_ASSERT_EQUAL(25, c.width);
    for (int len = 0; len <= 64; len++)
        for (int exp = 0; exp <= 24; exp++) {
            c = ui_code_plan(len, exp, 25);
            TEST_ASSERT_TRUE(c.width <= 25);
            TEST_ASSERT_TRUE(c.bullets <= len);
        }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_cuts_partition_the_rect);
    RUN_TEST(test_oversized_cut_takes_all_and_no_more);
    RUN_TEST(test_center_and_inset_stay_inside);
    RUN_TEST(test_metrics_keep_touch_heights_odd);
    RUN_TEST(test_grid_cells_tile_without_overlap);
    RUN_TEST(test_hub_tiles_fit_at_every_grid);
    RUN_TEST(test_section_pages_instead_of_overflowing);
    RUN_TEST(test_hit_resolves_a_cell_to_its_region);
    RUN_TEST(test_default_focus_skips_the_back_target);
    RUN_TEST(test_arrows_walk_the_grid);
    RUN_TEST(test_up_from_the_top_row_reaches_back);
    RUN_TEST(test_every_region_is_reachable_by_arrows);
    RUN_TEST(test_enter_presses_the_focused_region);
    RUN_TEST(test_tap_presses_and_hides_the_ring);
    RUN_TEST(test_tap_on_a_gutter_does_nothing);
    RUN_TEST(test_pressed_follows_the_finger_and_the_flash_expires);
    RUN_TEST(test_reset_drops_regions_and_focus);
    RUN_TEST(test_region_table_overflow_is_dropped_not_written);
    RUN_TEST(test_profile_limit_fits_or_pages);
    RUN_TEST(test_paging_counts_and_clamps);
    RUN_TEST(test_len_counts_code_points);
    RUN_TEST(test_one_decoder_walks_a_string);
    RUN_TEST(test_put_writes_what_the_decoder_reads);
    RUN_TEST(test_fit_counts_cells_not_bytes);
    RUN_TEST(test_number_words);
    RUN_TEST(test_centre_counts_an_uneven_split);
    RUN_TEST(test_rail_thumb_rounds_half_up);
    RUN_TEST(test_hold_key_holds_the_focused_region);
    RUN_TEST(test_release_after_a_hold_presses_nothing);
    RUN_TEST(test_focus_default_only_fills_an_empty_focus);
    RUN_TEST(test_debug_pose_forces_a_look);
    RUN_TEST(test_flow_wraps_breaks_and_cuts);
    RUN_TEST(test_own_widths);
    RUN_TEST(test_cols_grow_with_the_grid);
    RUN_TEST(test_arrow_prefers_the_next_row_straight_ahead);
    RUN_TEST(test_arrow_prefers_straight_ahead_to_the_side);
    RUN_TEST(test_entry_window_keeps_the_caret_in_view);
    RUN_TEST(test_text_insert_respects_caret_and_max);
    RUN_TEST(test_text_delete_takes_the_char_before_the_caret);
    RUN_TEST(test_code_plan_spaces_then_packs_then_clips);
    return UNITY_END();
}
