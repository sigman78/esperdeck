/*
 * app_ask.c — the question page (SCR_ASK): a text panel and two answers.
 * One instance: ask_open copies the strings, so callers pass temporaries.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

enum { ID_CANCEL = 1, ID_VERB };

static const char *TAG = "app_ask";

static struct {
    char    title[64], line1[64], line2[64], verb[24];
    uint8_t accent;
    void  (*yes)(intptr_t arg, uint64_t now);
    intptr_t arg;
} s_ask;

static void copy(char *dst, size_t sz, const char *src)
{
    snprintf(dst, sz, "%s", src ? src : "");
}

void ask_open(const ask_t *q, uint64_t now)
{
    copy(s_ask.title, sizeof s_ask.title, q->title);
    copy(s_ask.line1, sizeof s_ask.line1, q->line1);
    copy(s_ask.line2, sizeof s_ask.line2, q->line2);
    copy(s_ask.verb, sizeof s_ask.verb, q->verb);
    s_ask.accent = q->accent ? q->accent : OVERLAY_COL_RED;
    s_ask.yes = q->yes;
    s_ask.arg = q->arg;
    nav_push(SCR_ASK, 0, now);
}

static void render_ask(uint64_t now)
{
    (void)now;
    const int faults = ui_faults();
    ui_slate_t pg = ui_slate_page();
    ui_rect_t box = ui_center(pg.area, pg.area.w < 46 ? pg.area.w : 46, 9);
    const ui_rect_t card = ui_cut_top(&box, 6);
    const char *const line[2] = { s_ask.line1, s_ask.line2 };

    ui_focus_default(ID_CANCEL);
    ui_panel(card, s_ask.accent);
    ui_text(ui_rect(card.x + 2, card.y + 1, card.w - 4, 1), s_ask.title,
            UI_PANEL | UI_BOLD, UI_LEFT);
    for (int i = 0; i < 2; i++)
        ui_text(ui_rect(card.x + 2, card.y + 3 + i, card.w - 4, 1), line[i],
                UI_PANEL_MUTED, UI_LEFT);
    const ui_grid_t g = ui_grid(box, 2, 3, pg.m.gutter, 0);
    ui_push(ui_grid_cell(&g, 0), ID_CANCEL, "Cancel", UI_TONE_CARD, 0, 0);
    ui_push(ui_grid_cell(&g, 1), ID_VERB, s_ask.verb, UI_TONE_SOLID,
            s_ask.accent, 0);

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

static void ask_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                      uint64_t now)
{
    (void)ch;
    if (k == K_ESC) {
        nav_pop(now);
        return;
    }
    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind == UI_ACT_NONE) return;
    nav_invalidate();
    if (a.kind != UI_ACT_PRESS) return;
    if (a.id == ID_CANCEL) {
        nav_pop(now);
    } else if (a.id == ID_VERB) {
        /* Pop first: yes() may navigate anywhere. */
        void (*yes)(intptr_t, uint64_t) = s_ask.yes;
        const intptr_t arg = s_ask.arg;
        nav_pop(now);
        if (yes) yes(arg, now);
    }
}

const nav_screen_t ask_screen = {
    .name = "ask", .input = ask_input, .render = render_ask,
    .chrome = NAV_CHROME_FULL,
};
