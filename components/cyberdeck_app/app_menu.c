/*
 * app_menu.c - the setup screen (SCR_MENU): the session sheet, the setup
 * hub and the section pages, on the kit's parts. The item tables live in
 * app_menu_defs.c. Layout: docs/ui-kit.md.
 */

#include "app_internal.h"
#include "app_menu_defs.h"
#include "app_screens.h"
#include "app_settings.h"
#include "app_widgets.h"
#include "font.h"
#include "ssh_client.h"
#include "wifi_manager.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

/* Region ids. An item has ID_OPT + item * MENU_PICK_MAX + option. */
enum {
    ID_PREV = 1, ID_NEXT,
    ID_RESET = 10, ID_SETUP, ID_DISCONNECT,
    ID_HUB = 20,
    ID_OPT = 100,
    ID_ROW = 200,
};

#define SEC_ALL 0xFF   /* row page: shown on every page of a list */

/* One row of a section: the items on it, left to right. A danger row
 * holds danger items only and sits at the bottom of its page. */
typedef struct {
    uint8_t n, pg, danger;
    struct { uint8_t it, w; } c[MENU_ITEM_MAX];
} sec_row_t;

/* A section packed into rows and pages, as the last render saw it. Plain
 * rows come first, danger rows after; `ndanger` of the `nrows` are danger.
 * A list adds its tile area: offset, height, columns, tiles per page and
 * row count. */
typedef struct {
    sec_row_t rows[MENU_ITEM_MAX];
    int nrows, ndanger, per, pages, cap_w;
    int list_y, list_h, tcols, tper, ln;
} sec_lay_t;

static struct {
    int       screen;               /* menu_screen_t: page of the menu tree */
    /* root is the entry page. Back from here pops the screen: MS_MAIN
     * in-session, MS_CONFIG from HOME. */
    int       root;
    int       page;                 /* shown page of a section              */
    ui_id_t   saved_focus;          /* focus saved when another screen opens */
    ui_rect_t sheet;                /* the session sheet, as last drawn     */
    bool      fresh;                /* page switched, not drawn yet         */
    bool      follow;               /* resumed: show the focused item's page */
    sec_lay_t lay;
} s_menu;

static const char *TAG = "app_menu";

static bool is_section(int sc)
{
    return sc >= MS_HOSTS && sc < MS_COUNT;
}

static bool item_hidden(const menu_item_t *it)
{
    return it->hidden && it->hidden(it->arg);
}

static bool item_off(const menu_item_t *it)
{
    return it->off && it->off(it->arg);
}

static const char *item_label(const menu_item_t *it)
{
    return it->label_fn ? it->label_fn(it->arg) : it->label;
}

static ui_id_t item_id(int n, int opt)
{
    return (ui_id_t)(ID_OPT + n * MENU_PICK_MAX + opt);
}

/* The width an item keeps for itself. A choice takes the whole row. */
static int own_w(const menu_item_t *it)
{
    return it->kind == MENU_SWITCH
        ? ui_switch_w(item_label(it))
        : ui_act_w(item_label(it), it->nav ? UI_S_CHEVRON : "");
}

/* Pack the visible items of one class (danger or not) into rows of
 * @p room cells. A choice takes a row and the rest keep their own width.
 * Returns the row count. */
static int pack_rows(const menu_page_t *p, int room, int gx, bool danger,
                     sec_row_t *rows)
{
    int nrows = 0, used = 0;
    bool open = false;

    for (int n = 0; n < p->count; n++) {
        const menu_item_t *it = &p->items[n];
        if (item_hidden(it) || it->danger != danger) continue;
        const int own = own_w(it);
        const int w = it->kind == MENU_PICK ? room : own < room ? own : room;
        if (it->kind != MENU_PICK && open && used + gx + w <= room) {
            sec_row_t *r = &rows[nrows - 1];
            r->c[r->n].it = (uint8_t)n;
            r->c[r->n++].w = (uint8_t)w;
            used += gx + w;
            continue;
        }
        rows[nrows].n = 1;
        rows[nrows].c[0].it = (uint8_t)n;
        rows[nrows++].c[0].w = (uint8_t)w;
        open = it->kind != MENU_PICK;
        used = w;
    }
    return nrows;
}

/* Pack plain rows, then danger rows, and give each its page. Plain rows
 * fill pages top down. Danger rows take the last page, with one empty
 * row above. A full last page gives them a page of their own. */
static void lay_rows(const menu_page_t *p, int room, int gx, sec_lay_t *L)
{
    const int plain = pack_rows(p, room, gx, false, L->rows);
    L->ndanger = pack_rows(p, room, gx, true, L->rows + plain);
    L->nrows = plain + L->ndanger;

    const int pp = (plain + L->per - 1) / L->per;       /* plain pages */
    const int tail = plain - (pp ? (pp - 1) * L->per : 0);
    const int own = pp && L->ndanger && tail + 1 + L->ndanger > L->per;
    L->pages = pp + own > 0 ? pp + own : 1;
    for (int ri = 0; ri < L->nrows; ri++) {
        L->rows[ri].danger = ri >= plain;
        L->rows[ri].pg = (uint8_t)(ri < plain ? ri / L->per : L->pages - 1);
    }
}

/* A section with a list. Plain rows sit above the tiles on every page.
 * Danger rows hang below them on the last. Tiles take the rest of the
 * body, 4 rows each. The pages are pages of tiles. */
static void lay_list(const menu_page_t *p, ui_rect_t body, int gx,
                     sec_lay_t *L)
{
    const menu_list_t *ls = p->list;
    int plain = 0;

    L->ln = ls->count();
    for (int pass = 0; pass < 2; pass++) {
        const int room = body.w - 2 * pass;   /* the rail takes two cells */
        plain = pack_rows(p, room, gx, false, L->rows);
        L->ndanger = pack_rows(p, room, gx, true, L->rows + plain);
        L->nrows = plain + L->ndanger;
        L->list_y = plain ? plain * 3 + 1 : 0;
        L->list_h = body.h - L->list_y - (L->ndanger ? L->ndanger * 3 + 1 : 0);
        if (L->list_h < 0) L->list_h = 0;
        const ui_grid_t g = ui_grid(ui_rect(0, 0, room, L->list_h),
                                    ui_cols_for(room, ls->min_w, gx), 4, gx, 0);
        L->tcols = g.ncols;
        L->tper = ui_grid_capacity(&g);
        L->pages = ui_page_count(L->ln, L->tper);
        if (L->pages == 1) break;
    }
    for (int ri = 0; ri < L->nrows; ri++) {
        L->rows[ri].danger = ri >= plain;
        L->rows[ri].pg = (uint8_t)(ri < plain ? SEC_ALL : L->pages - 1);
    }
}

/* The one place that decides rows, pages and the caption column. Drawing,
 * the page flip and the focus after a resume all read the result. */
static void section_layout(const menu_page_t *p, ui_rect_t body, int gx,
                           sec_lay_t *L)
{
    L->per = body.h / 3 > 1 ? body.h / 3 : 1;
    if (p->list) {
        lay_list(p, body, gx, L);
    } else {
        lay_rows(p, body.w, gx, L);
        /* Paged rows lose two cells to the rail and its gap. */
        if (L->pages > 1) lay_rows(p, body.w - 2, gx, L);
    }
    L->cap_w = 0;
    for (int n = 0; n < p->count; n++) {
        const menu_item_t *it = &p->items[n];
        if (it->kind != MENU_PICK || item_hidden(it)) continue;
        const int w = ui_caption_w(item_label(it));
        if (w > L->cap_w) L->cap_w = w;
    }
}

/* The region a keyboard lands on when it enters item @p n. */
static ui_id_t item_entry(const menu_item_t *it, int n)
{
    const int ix = it->kind == MENU_PICK ? it->index(it->arg) : 0;
    return item_id(n, ix > 0 ? ix : 0);
}

static bool row_on(const sec_row_t *r, int page)
{
    return r->pg == SEC_ALL || r->pg == page;
}

static bool list_off(const menu_page_t *p)
{
    return p->list && p->list->off && p->list->off();
}

/* First target of the topmost row of @p page that has one, or 0. */
static ui_id_t row_entry(const menu_page_t *p, int page)
{
    const sec_lay_t *L = &s_menu.lay;
    for (int ri = 0; ri < L->nrows; ri++) {
        if (!row_on(&L->rows[ri], page)) continue;
        for (int ci = 0; ci < L->rows[ri].n; ci++) {
            const int n = L->rows[ri].c[ci].it;
            if (!item_off(&p->items[n])) return item_entry(&p->items[n], n);
        }
    }
    if (p->list && L->ln > 0 && !list_off(p)) return ID_ROW + page * L->tper;
    return 0;
}

static void draw_item(const menu_item_t *it, int n, ui_rect_t r, int cap_w)
{
    const ui_id_t id = item_id(n, 0);
    const uint8_t off = item_off(it) ? UI_PART_OFF : 0;

    if (it->kind == MENU_PICK) {
        const char *opt[MENU_PICK_MAX];
        int cnt = it->count(it->arg);
        if (cnt > MENU_PICK_MAX) cnt = MENU_PICK_MAX;
        for (int i = 0; i < cnt; i++) opt[i] = it->option(it->arg, i);
        ui_chips(r, id, item_label(it), opt, cnt, it->index(it->arg), 0,
                 cap_w);
    } else if (it->kind == MENU_SWITCH) {
        ui_switch(r, id, item_label(it), it->on(it->arg),
                  (uint8_t)(UI_PART_ROOMY | off));
    } else {
        ui_part(r, id,
                &(ui_part_t){ .title = item_label(it),
                              .right = it->nav ? UI_S_CHEVRON : NULL,
                              .tone = it->accent ? UI_TONE_TINT
                                                 : UI_TONE_CARD,
                              .accent = it->accent,
                              .flags = (uint8_t)(UI_PART_ROOMY | off) }, NULL);
    }
}

/* The list's tiles of the shown page in @p area, or its empty text. */
static void draw_list(const menu_list_t *ls, ui_rect_t area, int gx,
                      const sec_lay_t *L, int page)
{
    if (L->ln == 0) {
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_text(ui_rect(area.x + 1, area.y, area.w - 2, 1), ls->empty,
                UI_MUTED, UI_LEFT);
        return;
    }
    const ui_grid_t g = ui_grid(area, L->tcols, 4, gx, 0);
    const uint8_t off = ls->off && ls->off() ? UI_PART_OFF : 0;
    const int first = page * L->tper;
    for (int i = 0; i < L->tper && first + i < L->ln; i++) {
        char buf[128];
        ui_part_t t = { 0 };
        ls->row(first + i, &t, buf, sizeof buf);
        t.flags |= off;
        ui_part(ui_grid_cell(&g, i), (ui_id_t)(ID_ROW + first + i), &t, NULL);
    }
}

/* One section: a choice takes a row and shows every option. Switches and
 * actions keep their own width and pack from the left. A list follows. */
static void draw_section(const menu_page_t *p)
{
    ui_slate_t pg = ui_slate_page();
    sec_lay_t *L = &s_menu.lay;

    section_layout(p, pg.body, pg.m.gutter, L);
    if (s_menu.follow) {
        /* Resumed: land on the page that holds the focused item. A list
         * row that is gone leaves the focus on the last one. */
        s_menu.follow = false;
        ui_id_t f = ui_focus_get();
        if (p->list && f >= ID_ROW) {
            if (f - ID_ROW >= L->ln) {
                f = L->ln > 0 ? (ui_id_t)(ID_ROW + L->ln - 1) : ID_OPT;
                ui_focus_set(f);
            }
            if (f >= ID_ROW && L->tper > 0)
                s_menu.page = (f - ID_ROW) / L->tper;
        }
        for (int ri = 0; f >= ID_OPT && f < ID_ROW && ri < L->nrows; ri++)
            for (int ci = 0; ci < L->rows[ri].n; ci++)
                if ((f - ID_OPT) / MENU_PICK_MAX == L->rows[ri].c[ci].it &&
                    L->rows[ri].pg != SEC_ALL)
                    s_menu.page = L->rows[ri].pg;
    }
    s_menu.page = s_menu.page < 0 ? 0
                : s_menu.page >= L->pages ? L->pages - 1 : s_menu.page;
    ui_focus_default(row_entry(p, s_menu.page));

    const ui_rect_t head = ui_pages(pg.header, ID_PREV, ID_NEXT, s_menu.page,
                                    L->pages);
    ui_title(head, "Setup", p->title, NULL, true);
    if (L->pages > 1) {
        ui_rail(ui_cut_right(&pg.body, 1), s_menu.page, L->pages);
        ui_cut_right(&pg.body, 1);
    }
    const int gx = pg.m.gutter;
    int slot = 0, below = L->ndanger;
    if (p->list)
        draw_list(p->list, ui_rect(pg.body.x, pg.body.y + L->list_y,
                                   pg.body.w, L->list_h), gx, L, s_menu.page);
    for (int ri = 0; ri < L->nrows; ri++) {
        if (!row_on(&L->rows[ri], s_menu.page)) continue;
        /* Danger rows hang from the bottom edge of the body. */
        int y = pg.body.y + slot++ * 3;
        if (L->rows[ri].danger) {
            y = pg.body.y + pg.body.h - below-- * 3;
            if (y < pg.body.y) y = pg.body.y;
        }
        int x = pg.body.x;
        for (int ci = 0; ci < L->rows[ri].n; ci++) {
            const int n = L->rows[ri].c[ci].it, w = L->rows[ri].c[ci].w;
            draw_item(&p->items[n], n, ui_rect(x, y, w, 3), L->cap_w);
            x += w + gx;
        }
    }
}

/* The hub: a tile per section with its present state as the detail. */
static void draw_hub(void)
{
    ui_slate_t pg = ui_slate_page();
    int n;
    const uint8_t *hub = menu_hub(&n);

    ui_title(pg.header, s_menu.root == MS_MAIN ? "Session" : NULL, "Setup",
             NULL, true);
    const ui_grid_t g = ui_grid(pg.body,
                                ui_cols_for(pg.body.w, 24, pg.m.gutter), 4,
                                pg.m.gutter, 0);
    ui_focus_default(ID_HUB);
    for (int i = 0; i < n && i < ui_grid_capacity(&g); i++) {
        const menu_page_t *s = menu_page(hub[i]);
        char buf[64];
        const char *why = s->unavail ? s->unavail() : NULL;
        const char *detail = why ? why
                           : s->note ? s->note(buf, sizeof buf) : NULL;
        ui_part(ui_grid_cell(&g, i), (ui_id_t)(ID_HUB + i),
                &(ui_part_t){ .title = s->title, .detail = detail,
                              .right = UI_S_CHEVRON,
                              .flags = why ? UI_PART_OFF : 0 }, NULL);
    }
}

/* "42 s", "14 min" or "2 h 14 min": the units are separate words. */
static void fmt_span(char *buf, size_t sz, uint64_t s)
{
    if (s < 60)
        snprintf(buf, sz, "%u s", (unsigned)s);
    else if (s < 3600)
        snprintf(buf, sz, "%u min", (unsigned)(s / 60));
    else
        snprintf(buf, sz, "%u h %u min", (unsigned)(s / 3600),
                 (unsigned)(s / 60 % 60));
}

/* The session sheet: a slate panel centred over the dimmed terminal, above
 * the status row. Short grids drop the info lines, then the gaps. */
static void draw_sheet(void)
{
    const int cols = ui_cols(), rows = ui_rows();
    ui_dim();
    const ui_rect_t area = ui_rect(0, 0, cols, rows > 1 ? rows - 1 : rows);
    const bool info = area.h >= 18, gaps = area.h >= 15;
    const int g = gaps ? 1 : 0;
    const ui_rect_t sheet = ui_center(area, cols >= 32 ? 30 : cols,
                                      info ? 18 : gaps ? 15 : 11);
    s_menu.sheet = sheet;
    ui_fill(sheet.x, sheet.y, sheet.w, sheet.h, UI_TEXT);
    ui_rect_t inn = ui_inset(sheet, 1, 1);

    const conn_profile_t *cp = conn_active();
    ui_focus_default(ID_RESET);
    ui_title(ui_cut_top(&inn, 3), "Session",
             cp && cp->name[0] ? cp->name : "Connected", NULL, true);
    ui_part(ui_cut_top(&inn, 3), ID_RESET,
            &(ui_part_t){ .title = "Reset terminal" }, NULL);
    ui_part(ui_cut_top(&inn, 3), ID_SETUP,
            &(ui_part_t){ .title = "Setup", .right = UI_S_CHEVRON }, NULL);
    ui_cut_top(&inn, g);
    ui_push(ui_cut_top(&inn, 3), ID_DISCONNECT, "Disconnect", UI_TONE_TINT,
            OVERLAY_COL_RED, 0);
    if (!info) return;

    ui_cut_top(&inn, g);
    const uint64_t now_ms = (uint64_t)app.anim_frame * ANIM_PERIOD_MS;
    const uint64_t ss = conn_session_start();
    char up[32], span[32];
    fmt_span(up, sizeof up, now_ms / 1000);
    fmt_span(span, sizeof span, now_ms > ss ? (now_ms - ss) / 1000 : 0);
    const char *lab[2] = { "Uptime", "Session" };
    const char *val[2] = { up, span };
    const ui_rect_t txt = ui_inset(inn, 1, 0);
    const int lw = 9;
    for (int i = 0; i < 2; i++) {
        const int y = inn.y + i;
        ui_pen(OVERLAY_COL_CYAN);
        ui_text(ui_rect(txt.x, y, lw, 1), lab[i], UI_TEXT, UI_LEFT);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_units(ui_rect(txt.x + lw, y, txt.w - lw, 1), val[i], UI_TEXT,
                 UI_MUTED, UI_LEFT);
    }
}

static void render_menu(uint64_t now)
{
    (void)now;
    const int faults = ui_faults();
    const int sc = s_menu.screen;

    s_menu.fresh = false;
    if (sc == MS_MAIN)         draw_sheet();
    else if (sc == MS_CONFIG)  draw_hub();
    else if (is_section(sc))   draw_section(menu_page(sc));
    ui_pen(OVERLAY_COL_DEFAULT);

    /* Feedback renders on the StatusBar (the shared toast): the shell
     * composites it after this render. */
    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

/* Paint and present out of turn, for an action that blocks and reboots
 * (font apply), so its note is readable before the restart. */
void menu_present_now(uint64_t now)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    render_menu(now);
    ui_statusbar(now);       /* the note rides the StatusBar */
    ui_no_cursor();
    ui_present();
}

/* Menu feedback rides the shared toast on the StatusBar (ui-spec).
 * @p ms: lifetime; 0 = sticky. menu_exit clears a leftover sticky so it
 * never follows the user onto another screen. The shell's status tick
 * expires a timed note. app.toast_wifi marks a note that tracks Wi-Fi;
 * any new toast and the expiry drop the mark. */
void menu_note(uint64_t now, uint32_t ms, bool live_wifi, const char *text)
{
    toast_for(now, 1, "%s", text);
    app.toast_until = ms ? now + ms : UINT64_MAX;   /* MAX = sticky */
    app.toast_wifi  = live_wifi;
}

static void menu_clear_note(void)
{
    app.toast[0]   = '\0';
    app.toast_wifi = false;
}

/* Switch to menu screen @p sc: first page, no saved focus, no note. The
 * page snapshots volatile state in on_open. The settings hold defers flash
 * writes while its page is open (see app_settings_idle_flush). */
void menu_goto(int sc)
{
    s_menu.screen      = sc;
    s_menu.page        = 0;
    s_menu.saved_focus = 0;
    s_menu.follow      = false;
    s_menu.fresh       = true;
    ui_hot_reset();
    const menu_page_t *p = menu_page(sc);
    if (p && p->on_open) p->on_open();
    app_settings_hold(p ? p->hold : 0);
    menu_clear_note();
    nav_invalidate();
}

/* Back one level. Back at the entry root leaves the menu screen; anywhere
 * else it follows the page's back_to, with the focus on the way we came. */
void menu_back(uint64_t now)
{
    const int from = s_menu.screen;
    const int to = menu_page(from)->back_to;
    if (from == s_menu.root || to == MENU_BACK_LEAVE) {
        nav_pop(now);
        return;
    }
    menu_goto(to);
    if (to == MS_MAIN) {
        ui_focus_set(ID_SETUP);
    } else if (to == MS_CONFIG) {
        int n;
        const uint8_t *hub = menu_hub(&n);
        for (int i = 0; i < n; i++)
            if (hub[i] == from) ui_focus_set((ui_id_t)(ID_HUB + i));
    }
}

/* @p arg = the root page: MS_MAIN in-session, MS_CONFIG from HOME. */
static void menu_enter(intptr_t arg, uint64_t now)
{
    (void)now;
    s_menu.root = (int)arg;
    menu_goto((int)arg);
}

/* Revealed by a pop (question page, code pad, pairing, import): land on
 * the page and the item the user left. */
static void menu_resume(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    const menu_page_t *p = menu_page(s_menu.screen);
    if (p && p->on_open) p->on_open();
    app_settings_hold(p ? p->hold : 0);
    s_menu.fresh  = true;
    s_menu.follow = true;
    if (s_menu.saved_focus) ui_focus_set(s_menu.saved_focus);
    nav_invalidate();
}

/* Any departure, a pop or another screen pushed on top, releases the
 * settings hold. Deferred writes then flush on the next tick. It also
 * drops a leftover STICKY note. */
static void menu_exit(uint64_t now)
{
    (void)now;
    s_menu.saved_focus = ui_focus_get();
    app_settings_hold(0);
    if (app.toast_until == UINT64_MAX) menu_clear_note();
}

void menu_open(uint64_t now)
{
    nav_push(SCR_MENU, MS_MAIN, now);
}

void menu_open_config(uint64_t now)
{
    nav_push(SCR_MENU, MS_CONFIG, now);
}

static void menu_tick(uint64_t now)
{
    /* Only the in-session menu has a session to monitor. */
    if (s_menu.root == MS_MAIN && !ssh_client_is_connected()) {
        session_dropped(now);
        return;
    }
    /* Live state runs on the 10 fps gate: the hub notes, the sheet clocks
     * and the wifi-tracking note. Expired notes clear. */
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        if (app.toast_wifi && app.toast[0]) {
            snprintf(app.toast, sizeof(app.toast), "wifi: %s",
                     wifi_status_str());
            /* Still in flight? Keep the note alive: expiring mid-
             * reconnect reads as the action silently dying. */
            wifi_mgr_state_t ws = wifi_manager_get_state();
            if (ws == WIFI_MGR_CONNECTING || ws == WIFI_MGR_LOST)
                app.toast_until = now + MENU_MSG_MS;
            else if (now >= app.toast_until)
                menu_clear_note();
        }
        nav_invalidate();
    }
}

/* Step the page of a section; focus lands on the first item of the new one. */
static void menu_flip(int dir)
{
    const sec_lay_t *L = &s_menu.lay;
    const int page = s_menu.page + dir;
    if (!is_section(s_menu.screen) || page < 0 || page >= L->pages) return;
    s_menu.page = page;
    const menu_page_t *p = menu_page(s_menu.screen);
    /* A list page lands on its first tile, as HOME does. */
    ui_focus_set(p->list && L->ln > 0 && !list_off(p)
                 ? (ui_id_t)(ID_ROW + page * L->tper) : row_entry(p, page));
    nav_invalidate();
}

/* A hit on an item: a switch flips, a choice picks, an action runs or asks. */
static void item_press(const menu_item_t *it, int opt, uint64_t now)
{
    if (item_off(it)) return;
    if (it->kind == MENU_PICK) {
        if (it->set) it->set(it->arg, opt, now);
    } else if (it->kind == MENU_ACT && it->ask) {
        ask_open(&(ask_t){
            .title = it->ask,
            .line1 = it->ask_line ? it->ask_line : "This cannot be undone.",
            .verb = item_label(it),
            .yes = it->action, .arg = it->arg,
        }, now);
    } else if (it->action) {
        it->action(it->arg, now);
    }
}

static void menu_press(ui_id_t id, uint64_t now)
{
    switch (id) {
    case UI_ID_BACK:      menu_back(now);                 return;
    case ID_PREV:         menu_flip(-1);                  return;
    case ID_NEXT:         menu_flip(1);                   return;
    case ID_RESET:        menu_act_reset_terminal(now);   return;
    case ID_SETUP:        menu_goto(MS_CONFIG);           return;
    case ID_DISCONNECT:   menu_act_disconnect(now);       return;
    default: break;
    }
    if (id >= ID_ROW) {
        const menu_page_t *p = menu_page(s_menu.screen);
        const int i = id - ID_ROW;
        if (is_section(s_menu.screen) && p->list && !list_off(p) &&
            i < s_menu.lay.ln)
            p->list->open(i, now);
    } else if (id >= ID_OPT) {
        const menu_page_t *p = menu_page(s_menu.screen);
        const int n = (id - ID_OPT) / MENU_PICK_MAX;
        if (is_section(s_menu.screen) && n < p->count)
            item_press(&p->items[n], (id - ID_OPT) % MENU_PICK_MAX, now);
    } else if (id >= ID_HUB && s_menu.screen == MS_CONFIG) {
        int n;
        const uint8_t *hub = menu_hub(&n);
        const int i = id - ID_HUB;
        const menu_page_t *s = i < n ? menu_page(hub[i]) : NULL;
        if (s && !(s->unavail && s->unavail())) menu_goto(hub[i]);
    }
}

static void menu_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                       uint64_t now)
{
    (void)ch;
    /* Esc and F12 step back one level. */
    if (k == K_ESC || k == K_F12) {
        menu_back(now);
        return;
    }
    /* A tap that lands before the page's first frame has no regions to
     * hit: drop it. */
    if (s_menu.fresh) return;
    if (k == K_SCROLL_UP || k == K_SCROLL_DOWN) {
        menu_flip(k == K_SCROLL_UP ? -1 : 1);
        return;
    }
    /* An arrow at the edge of a page of tiles turns it (ui_page_step). */
    const sec_lay_t *L = &s_menu.lay;
    const ui_id_t f = ui_focus_get();
    if (is_section(s_menu.screen) && menu_page(s_menu.screen)->list &&
        f >= ID_ROW && ui_focused(f)) {
        const int to = ui_page_step(k, f - ID_ROW, L->ln, L->tcols, L->tper);
        if (to >= 0) {
            s_menu.page = to / L->tper;
            ui_focus_set((ui_id_t)(ID_ROW + to));
            nav_invalidate();
            return;
        }
    }

    const ui_act_t a = app_hot_input(ev, k);
    /* On the sheet a tap outside its rect resumes the session. */
    if (ev->type == CYBERDECK_INPUT_TAP && !a.id && s_menu.screen == MS_MAIN) {
        const int cx = ev->x / font_width(), cy = ev->y / font_height();
        const ui_rect_t *s = &s_menu.sheet;
        if (cx < s->x || cx >= s->x + s->w || cy < s->y || cy >= s->y + s->h) {
            menu_back(now);
            return;
        }
    }
    if (a.kind == UI_ACT_NONE) return;
    nav_invalidate();
    if (a.kind == UI_ACT_PRESS) menu_press(a.id, now);
}

const nav_screen_t menu_screen = {
    .name = "menu", .enter = menu_enter, .resume = menu_resume,
    .exit = menu_exit, .tick = menu_tick, .input = menu_input,
    .render = render_menu, .chrome = NAV_CHROME_FULL,
};
