/*
 * uilab_mo.c — the lab's slate screens, drawn with the kit's parts.
 * Each screen ports a sketch in tools/uimock/sketches-mo.js and carries
 * the name of its MO_GATE frame. The gate compares both cell by cell.
 */

#include "uilab.h"
#include "ui_pure.h"

#include <stdio.h>
#include <string.h>

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

enum {
    MO_HOST = 2000, MO_PILL = 2100, MO_PREV = 2200, MO_NEXT, MO_HUB = 2300,
    MO_OPT = 2400, MO_BTN = 2600, MO_KEY = 2700, MO_FIELD = 2800, MO_ROW = 2900,
};

/* Status row: the chips on a panel-tone bar, then a hint. The hint names
 * keys once a key press has shown the focus ring. A red alert replaces it. */
static void mo_status(ui_rect_t r, const char *touch, const char *keys,
                      const char *alert)
{
    static const uint16_t STAIR[3] = { 0x2582, 0x2584, 0x2586 };
    const int y = r.y;
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_fill(r.x, r.y, r.w, r.h, UI_PANEL);
    int x = r.x + 1;
    ui_pen(OVERLAY_COL_GREEN);
    ui_puts(x, y, " NET ", UI_FOCUS | UI_BOLD);
    x += 5;
    for (int i = 0; i < 3; i++) ui_putch(x++, y, STAIR[i], UI_FOCUS | UI_BOLD);
    ui_putch(x++, y, ' ', UI_FOCUS);
    x += 1;
    ui_pen(OVERLAY_COL_CYAN);
    ui_puts(x, y, " KBD ", UI_FOCUS | UI_BOLD);
    x += 5;
    ui_pen(OVERLAY_COL_AMBER);
    ui_putch(x++, y, 'C', UI_FOCUS | UI_BOLD);
    ui_pen(OVERLAY_COL_CYAN);
    ui_puts(x, y, "N ", UI_FOCUS);
    x += 2;
    const int clock = r.x + r.w - 8;
    const ui_rect_t room = ui_rect(x + 2, y, clock - x - 3, 1);
    const bool ring = ui_focused(ui_focus_get());
    const char *hint = ring && keys && *keys ? keys : touch;
    if (alert && *alert) {
        ui_pen(OVERLAY_COL_RED);
        ui_text(room, alert, UI_PANEL | UI_BOLD, UI_LEFT);
    } else if (hint && *hint) {
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_text(room, hint, UI_PANEL_MUTED, UI_LEFT);
    }
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(clock, y, " 14:07 ", UI_FOCUS | UI_BOLD);
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* Every part in its states: normal, finger down, key focus, off. */
static void scr_parts(void)
{
    static const char *const HEAD[4] = { "normal", "finger down", "key focus",
                                         "off" };
    static const int POSE[4] = { 0, UI_POSE_DOWN, UI_POSE_FOCUS, 0 };
    static const char *const TINT[2] = { "color", "green" };
    const int cols = ui_cols(), rows = ui_rows();
    const int cw = (cols - 2 - 3) / 4;
#define COL(i, y, h) ui_rect(1 + (i) * (cw + 1), (y), cw, (h))

    ui_slate();
    ui_pen(OVERLAY_COL_DEFAULT);
    for (int i = 0; i < 4; i++)
        ui_text(ui_rect(1 + i * (cw + 1) + 1, 0, cw - 1, 1), HEAD[i], UI_MUTED,
                UI_LEFT);
    int y = 1;
    for (int i = 0; i < 4; i++) {
        if (POSE[i]) ui_debug_pose((ui_id_t)(1 + i), POSE[i]);
        ui_part(COL(i, y, 4), (ui_id_t)(1 + i),
                &(ui_part_t){ .title = "nas", .detail = "pi@nas.lan",
                              .stripe = OVERLAY_COL_GREEN,
                              .flags = i == 3 ? UI_PART_OFF : 0 }, NULL);
    }
    y += 4;
    for (int i = 0; i < 4; i++) {
        if (POSE[i]) ui_debug_pose((ui_id_t)(11 + i), POSE[i]);
        ui_push(COL(i, y, 3), (ui_id_t)(11 + i), "Connect", UI_TONE_SOLID,
                OVERLAY_COL_GREEN, i == 3 ? UI_PART_OFF : 0);
    }
    y += 3;
    for (int i = 0; i < 4; i++) {
        if (POSE[i]) ui_debug_pose((ui_id_t)(21 + i), POSE[i]);
        ui_switch(COL(i, y, 3), (ui_id_t)(21 + i), "FX", i != 0,
                  i == 3 ? UI_PART_OFF : 0);
    }
    y += 3;
    if (rows - y >= 6) {
        const int half = (cols - 3) / 2;
        ui_debug_pose(32, UI_POSE_FOCUS);
        ui_chips(ui_rect(1, y, half, 3), 31, "Tint", TINT, 2, 0, 0);
        ui_debug_pose(35, UI_POSE_FOCUS);
        ui_debug_pose(36, UI_POSE_DOWN);
        ui_chips(ui_rect(2 + half, y, half, 3), 35, "Tint", TINT, 2, 0, 0);
        y += 3;
    }
    if (rows - y >= 3) {
        const ui_rect_t rest = ui_pages(ui_rect(1, y, cols - 2, 3), MO_PREV,
                                        MO_NEXT, 1, 3);
        const int h = (rest.w - 1) / 2;
        ui_part(ui_rect(rest.x, y, h, 3), 41,
                &(ui_part_t){ .title = "HomeAP", .right = "-55 dBm",
                              .flags = UI_PART_UNITS }, NULL);
        ui_push(ui_rect(rest.x + h + 1, y, rest.w - h - 1, 3), 42, "Delete",
                UI_TONE_TINT, OVERLAY_COL_RED, 0);
        y += 3;
    }
    if (rows - y >= 1)
        mo_status(ui_rect(0, rows - 1, cols, 1), "Hold a host for options", "",
                  NULL);
#undef COL
}

/* The host form in a known state. */
typedef struct {
    char name[40], user[40], host[40], port[12], pass[40];
    bool key, show;
} form_t;

typedef struct {
    bool is_new;
    form_t f;
    form_t was;               /* the form as loaded: what "changed" means */
    int edit;                 /* index of the field in typing, else -1    */
} edit_t;

static form_t base_form(void)
{
    return (form_t){ "homelab", "root", "192.168.1.10", "22", "hunter22" };
}

/* "user@host:port  key" in parts, as moParse does. */
static form_t form_of(const lab_profile_t *p)
{
    form_t f = { 0 };
    const char *at = strchr(p->where, '@'), *colon = strchr(p->where, ':');
    snprintf(f.name, sizeof f.name, "%s", p->name);
    snprintf(f.user, sizeof f.user, "%.*s", (int)(at - p->where), p->where);
    snprintf(f.host, sizeof f.host, "%.*s", (int)(colon - at - 1), at + 1);
    snprintf(f.pass, sizeof f.pass, "hunter22");
    f.key = strstr(colon, "key") != NULL;
    colon++;
    snprintf(f.port, sizeof f.port, "%.*s", (int)strspn(colon, "0123456789"),
             colon);
    return f;
}

static bool same_form(const form_t *a, const form_t *b)
{
    return !strcmp(a->name, b->name) && !strcmp(a->user, b->user) &&
           !strcmp(a->host, b->host) && !strcmp(a->port, b->port) &&
           !strcmp(a->pass, b->pass) && a->key == b->key && a->show == b->show;
}

static bool port_ok(const char *s)
{
    int v = 0;
    if (!*s) return false;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return false;
        v = v * 10 + (*s - '0');
        if (v > 65535) return false;
    }
    return v >= 1;
}

static void draw_edit(const edit_t *st)
{
    static const char *const AUTH[2] = { "password", "key" };
    const ui_slate_t pg = ui_slate_page();
    const form_t *f = &st->f;
    const ui_rect_t body = pg.body;
    const int gx = pg.m.gutter;

    ui_focus_default(MO_FIELD);
    const bool changed = !same_form(f, &st->was);
    const bool port_bad = !port_ok(f->port);
    const bool name_bad = !f->name[0], host_bad = !f->host[0];
    /* An untouched new form shows no red yet. Save stays off until valid. */
    const char *msg_port = port_bad ? "Port: a number from 1 to 65535." : NULL;
    const char *msg_name = name_bad && changed ? "A host needs a name." : NULL;
    const char *msg_host = host_bad && changed ? "A host needs an address." : NULL;
    const bool wrong = port_bad || name_bad || host_bad;
    const char *alert = msg_port ? msg_port : msg_name ? msg_name : msg_host;

    char title[64];
    if (st->is_new) snprintf(title, sizeof title, "New host");
    else            snprintf(title, sizeof title, "Edit %s", f->name);
    ui_rect_t head = pg.header;
    ui_pill(&head, MO_PILL, "Save", UI_TONE_SOLID, OVERLAY_COL_GREEN,
            wrong ? UI_PART_OFF : 0);
    ui_title(head, "Hosts", title, NULL, true);

    const int half = (body.w - gx) >> 1, port_w = 17, wide = body.w - gx - port_w;
    const struct { const char *label, *value; int lw; bool mask; const char *bad; }
    fld[5] = {
        { "Name", f->name, 0, false, msg_name },
        { "User", f->user, 0, false, NULL },
        { "Host", f->host, 0, false, msg_host },
        { "Port", f->port, 6, false, msg_port },
        { "Password", f->pass, 0, !f->show, NULL },
    };
    const ui_rect_t cell[4] = {
        ui_rect(body.x, body.y, half, 3),
        ui_rect(body.x + half + gx, body.y, body.w - half - gx, 3),
        ui_rect(body.x, body.y + 3, wide, 3),
        ui_rect(body.x + wide + gx, body.y + 3, port_w, 3),
    };
    for (int i = 0; i < 4; i++)
        ui_entry(cell[i], (ui_id_t)(MO_FIELD + i), fld[i].label, fld[i].value,
                 fld[i].lw,
                 (uint8_t)((st->edit == i ? UI_ENTRY_EDIT : 0) |
                           (fld[i].mask ? UI_ENTRY_MASK : 0) |
                           (fld[i].bad ? UI_ENTRY_BAD : 0)));
    int y = body.y + 6;
    ui_chips(ui_rect(body.x, y, body.w, 3), MO_OPT, "Sign-in", AUTH, 2,
             f->key ? 1 : 0, 0);
    y += 3;
    if (!f->key) {
        ui_entry(ui_rect(body.x, y, half, 3), MO_FIELD + 4, fld[4].label,
                 fld[4].value, fld[4].lw,
                 (uint8_t)((st->edit == 4 ? UI_ENTRY_EDIT : 0) |
                           (fld[4].mask ? UI_ENTRY_MASK : 0)));
        ui_switch(ui_rect(body.x + half + gx, y, body.w - half - gx, 3),
                  MO_OPT + 4, "Show password", f->show, 0);
    } else {
        ui_part(ui_rect(body.x, y, half, 3), MO_OPT + 5,
                &(ui_part_t){ .title = "Key",
                              .right = "deck-ed25519 " UI_S_CHEVRON,
                              .flags = UI_PART_RIGHT_BOLD }, NULL);
    }
    if (st->edit >= 0)
        mo_status(pg.status, "Type on the keyboard.",
                  "type  " UI_S_ENTER " done  " UI_S_UP UI_S_DOWN " leave",
                  alert);
    else
        mo_status(pg.status, "Tap a field to change it.",
                  UI_S_ENTER " change  Esc cancel", alert);
}

static edit_t edit_state(int n)
{
    edit_t e = { .edit = -1 };
    e.f = e.was = form_of(&PROFILES[0]);
    switch (n) {
    case 1:                                    /* a new, empty form      */
        e.is_new = true;
        e.f = e.was = (form_t){ .port = "22" };
        break;
    case 2:                                    /* typing in the host     */
        e.f = e.was = base_form();
        snprintf(e.f.host, sizeof e.f.host, "192.168.1.1");
        e.f.show = true;
        e.edit = 2;
        break;
    case 3:                                    /* two bad values         */
        e.f = e.was = base_form();
        e.f.name[0] = 0;
        snprintf(e.f.port, sizeof e.f.port, "70000");
        break;
    case 4:                                    /* key sign-in            */
        e.f = e.was = base_form();
        e.f.key = true;
        break;
    default: break;
    }
    return e;
}

static void draw_edit_n(int n)
{
    const edit_t e = edit_state(n);
    draw_edit(&e);
}

static void scr_edit(void)        { draw_edit_n(0); }
static void scr_edit_new(void)    { draw_edit_n(1); }
static void scr_edit_typing(void) { draw_edit_n(2); }
static void scr_edit_bad(void)    { draw_edit_n(3); }
static void scr_edit_key(void)    { draw_edit_n(4); }

/* "user@host:port  key" in parts, as moParse does. */
typedef struct { char user[40], host[48], port[12]; bool key; } where_t;

static where_t parse_where(const char *w)
{
    where_t o = { .port = "22" };
    const char *at = strchr(w, '@');
    const char *colon = at ? strchr(at + 1, ':') : NULL;
    const int digits = colon ? (int)strspn(colon + 1, "0123456789") : 0;
    if (!at || at == w || colon == at + 1 || !digits) {
        snprintf(o.host, sizeof o.host, "%s", w);
        return o;
    }
    snprintf(o.user, sizeof o.user, "%.*s", (int)(at - w), w);
    snprintf(o.host, sizeof o.host, "%.*s", (int)(colon - at - 1), at + 1);
    snprintf(o.port, sizeof o.port, "%.*s", digits, colon + 1);
    const char *rest = colon + 1 + digits;
    const int space = (int)strspn(rest, " \t");
    o.key = space > 0 && !strncmp(rest + space, "key", 3);
    return o;
}

/* HOME: a host per tile. A tap connects. A hold opens the host page. */
static void scr_home(void)
{
    ui_slate_t pg = ui_slate_page();
    char note[16];

    ui_focus_default(MO_HOST);
    ui_pill(&pg.header, MO_PILL + 2, "Lock", UI_TONE_CARD, 0, 0);
    ui_pill(&pg.header, MO_PILL + 1, "Setup", UI_TONE_CARD, 0, 0);
    ui_pill(&pg.header, MO_PILL, "+ New", UI_TONE_CARD, 0, 0);
    const ui_grid_t g = ui_grid(pg.body, 2, 4, pg.m.gutter, 0);
    const int per = ui_grid_capacity(&g);
    const int pages = ui_page_count(LAB_NPROFILES, per);
    const ui_rect_t rest = ui_pages(pg.header, MO_PREV, MO_NEXT, 0, pages);
    snprintf(note, sizeof note, "%d of 8", LAB_NPROFILES);
    ui_title(rest, NULL, "Hosts", note, false);
    for (int i = 0; i < per && i < LAB_NPROFILES; i++) {
        const where_t w = parse_where(PROFILES[i].where);
        char addr[80], num[4];
        if (!strcmp(w.port, "22")) snprintf(addr, sizeof addr, "%s@%s", w.user, w.host);
        else snprintf(addr, sizeof addr, "%s@%s:%s", w.user, w.host, w.port);
        snprintf(num, sizeof num, "%d", i + 1);
        ui_part(ui_grid_cell(&g, i), (ui_id_t)(MO_HOST + i),
                &(ui_part_t){ .title = PROFILES[i].name, .detail = addr,
                              .stripe = prof_accent(PROFILES[i].name),
                              .right = num }, NULL);
    }
    mo_status(pg.status, "Tap to connect. Hold for options.",
              UI_S_ENTER " connect  Space options  1-8", NULL);
}

/* The host page: what a hold on a tile opens. */
static void draw_host(int n)
{
    ui_slate_t pg = ui_slate_page();
    const lab_profile_t *p = &PROFILES[n];
    const where_t w = parse_where(p->where);
    char addr[80];

    ui_title(pg.header, "Hosts", p->name, NULL, true);
    snprintf(addr, sizeof addr, "%s  port %s", w.host, w.port);
    const ui_info_row_t rows[4] = {
        { "Address", addr }, { "User", w.user },
        { "Sign-in", w.key ? "key deck-ed25519" : "password, stored" },
        { "Host key", "SHA256:9fK2..Qm4c", 0, 0, UI_S_CHECK " pinned",
          OVERLAY_COL_GREEN },
    };
    ui_info(ui_cut_top(&pg.body, 6), rows, 4, prof_accent(p->name));
    ui_push(ui_cut_top(&pg.body, 3), MO_BTN, "Connect", UI_TONE_SOLID,
            OVERLAY_COL_GREEN, 0);
    const ui_grid_t g = ui_grid(pg.body, 2, 3, pg.m.gutter, 0);
    ui_push(ui_grid_cell(&g, 0), MO_BTN + 1, "Edit", UI_TONE_CARD, 0, 0);
    ui_push(ui_grid_cell(&g, 1), MO_BTN + 2, "Delete", UI_TONE_TINT,
            OVERLAY_COL_RED, 0);
    ui_push(ui_grid_cell(&g, 2), MO_BTN + 3, UI_S_UP " Move earlier",
            UI_TONE_CARD, 0, 0);
    ui_push(ui_grid_cell(&g, 3), MO_BTN + 4, UI_S_DOWN " Move later",
            UI_TONE_CARD, 0, 0);
    mo_status(pg.status, "The bar at the top goes back.",
              UI_S_ENTER " choose  Esc back", NULL);
}

static void scr_host(void)     { draw_host(0); }
static void scr_host_key(void) { draw_host(3); }

/* A question as its own page: a text panel, then the two answers. */
typedef struct {
    const char *title, *verb, *line[2];
    uint8_t accent;
} ask_t;

static void draw_confirm(const ask_t *ask)
{
    ui_slate_t pg = ui_slate_page();
    ui_rect_t box = ui_center(pg.area, 46, 9);
    const ui_rect_t card = ui_cut_top(&box, 6);

    ui_panel(card, ask->accent);
    ui_text(ui_rect(card.x + 2, card.y + 1, card.w - 4, 1), ask->title,
            UI_PANEL | UI_BOLD, UI_LEFT);
    for (int i = 0; i < 2; i++)
        ui_text(ui_rect(card.x + 2, card.y + 3 + i, card.w - 4, 1),
                ask->line[i], UI_PANEL_MUTED, UI_LEFT);
    const ui_grid_t g = ui_grid(box, 2, 3, pg.m.gutter, 0);
    ui_push(ui_grid_cell(&g, 0), MO_BTN, "Cancel", UI_TONE_CARD, 0, 0);
    ui_push(ui_grid_cell(&g, 1), MO_BTN + 1, ask->verb, UI_TONE_SOLID,
            ask->accent, 0);
    mo_status(pg.status, "",
              UI_S_LEFT UI_S_RIGHT " move  " UI_S_ENTER " choose  Esc cancel",
              NULL);
}

static void scr_confirm(void)
{
    char title[64];
    snprintf(title, sizeof title, "Delete %s?", PROFILES[0].name);
    draw_confirm(&(ask_t){ title, "Delete",
                           { "The entry and its stored password go.",
                             "The pinned host key stays." },
                           OVERLAY_COL_RED });
}

static void scr_confirm_ask(void)
{
    draw_confirm(&(ask_t){ "Discard the changes?", "Discard",
                           { "This host keeps its old values.", "" },
                           OVERLAY_COL_AMBER });
}

/* Setup hub: a section per tile, with its present state as the detail. */
enum { IT_ACT, IT_TOGGLE, IT_PICK };

typedef struct {
    uint8_t     type;
    const char *label;
    bool        nav;                  /* opens another page            */
    uint8_t     accent;               /* a caution action              */
    int         init;                 /* switch state or chosen option */
    int         nset;
    const char *set[5];
} item_t;

typedef struct { const char *name, *note; int n; item_t it[4]; } cat_t;

static const cat_t MO_SETUP[] = {
    { "Hosts", "8 stored", 2, {
        { IT_ACT, "Import from phone", .nav = true },
        { IT_ACT, "Import from PC", .nav = true } } },
    { "Wi-Fi", "HomeAP  -55 dBm", 2, {
        { IT_ACT, "Reconnect" },
        { IT_ACT, "Add network (phone)", .nav = true } } },
    { "Keyboard", "K380 connected", 2, {
        { IT_ACT, "Pair keyboard", .nav = true },
        { IT_ACT, "Forget bonds", .accent = OVERLAY_COL_RED } } },
    { "CRT FX", "scanlines, phosphor", 3, {
        { IT_TOGGLE, "Scanlines", .init = 1 },
        { IT_TOGGLE, "Bold pop", .init = 1 },
        { IT_PICK, "Phosphor", .init = 0, .nset = 3,
          .set = { "color", "green", "amber" } } } },
    { "Motion FX", "wobble, wipe, static", 4, {
        { IT_PICK, "Wobble", .init = 2, .nset = 4,
          .set = { "off", "subtle", "medium", "deep" } },
        { IT_PICK, "Wipe in", .init = 1, .nset = 3,
          .set = { "off", "0.3 s", "0.6 s" } },
        { IT_PICK, "Collapse", .init = 1, .nset = 3,
          .set = { "off", "0.2 s", "0.5 s" } },
        { IT_PICK, "Static", .init = 1, .nset = 3,
          .set = { "off", "light", "heavy" } } } },
    { "Font", "12x24  66x20", 1, {
        { IT_PICK, "Size (restarts the deck)", .init = 2, .nset = 3,
          .set = { "8x16", "10x20", "12x24" } } } },
    { "System", "saver, reset", 4, {
        { IT_PICK, "Saver + lock, min", .init = 2, .nset = 5,
          .set = { "1", "3", "5", "10", "30" } },
        { IT_TOGGLE, "Edge scroll", .init = 0 },
        { IT_ACT, "Clear host keys", .accent = OVERLAY_COL_RED },
        { IT_ACT, "Factory reset", .accent = OVERLAY_COL_RED } } },
    { "Keystore", "unlocked", 3, {
        { IT_ACT, "Lock deck" }, { IT_ACT, "Change code", .nav = true },
        { IT_ACT, "Remove code", .nav = true, .accent = OVERLAY_COL_RED } } },
};

static void scr_setup(void)
{
    ui_slate_t pg = ui_slate_page();

    ui_title(pg.header, NULL, "Setup", NULL, true);
    const ui_grid_t g = ui_grid(pg.body, 2, 4, pg.m.gutter, 0);
    for (int i = 0; i < NELEM(MO_SETUP); i++)
        ui_part(ui_grid_cell(&g, i), (ui_id_t)(MO_HUB + i),
                &(ui_part_t){ .title = MO_SETUP[i].name,
                              .detail = MO_SETUP[i].note,
                              .right = UI_S_CHEVRON }, NULL);
    mo_status(pg.status, "The bar at the top goes back.",
              UI_S_ENTER " open  Esc back", NULL);
}

/* One section. Switches and actions share a row of two. A choice takes
 * a full row and shows every option. */
static void draw_section(int cat)
{
    ui_slate_t pg = ui_slate_page();
    const cat_t *c = &MO_SETUP[cat];
    struct { int n, it[2]; } rows[4];
    int nrows = 0;
    bool open = false;

    ui_focus_default(MO_OPT);
    for (int n = 0; n < c->n; n++) {
        if (c->it[n].type == IT_PICK) {
            rows[nrows].n = 1;
            rows[nrows++].it[0] = n;
            open = false;
        } else if (open) {
            rows[nrows - 1].it[1] = n;
            rows[nrows - 1].n = 2;
            open = false;
        } else {
            rows[nrows].n = 1;
            rows[nrows++].it[0] = n;
            open = true;
        }
    }
    const int per = pg.body.h / 3 > 1 ? pg.body.h / 3 : 1;
    const int pages = ui_page_count(nrows, per);
    const ui_rect_t head = ui_pages(pg.header, MO_PREV, MO_NEXT, 0, pages);
    ui_title(head, "Setup", c->name, NULL, true);
    if (pages > 1) {
        ui_rail(ui_cut_right(&pg.body, 1), 0, pages);
        ui_cut_right(&pg.body, 1);
    }
    const ui_rect_t body = pg.body;
    const int gx = pg.m.gutter, half = (body.w - gx) >> 1;
    for (int ri = 0; ri < per && ri < nrows; ri++) {
        const int y = body.y + ri * 3;
        for (int ci = 0; ci < rows[ri].n; ci++) {
            const int n = rows[ri].it[ci];
            const item_t *it = &c->it[n];
            const ui_id_t id = (ui_id_t)(MO_OPT + n * 8);
            const ui_rect_t r = it->type == IT_PICK
                ? ui_rect(body.x, y, body.w, 3)
                : ui_rect(body.x + ci * (half + gx), y,
                          ci ? body.w - half - gx : half, 3);
            if (it->type == IT_PICK)
                ui_chips(r, id, it->label, it->set, it->nset, it->init, 0);
            else if (it->type == IT_TOGGLE)
                ui_switch(r, id, it->label, it->init != 0, 0);
            else
                ui_part(r, id,
                        &(ui_part_t){ .title = it->label,
                                      .right = it->nav ? UI_S_CHEVRON : NULL,
                                      .tone = it->accent ? UI_TONE_TINT
                                                         : UI_TONE_CARD,
                                      .accent = it->accent }, NULL);
        }
    }
    mo_status(pg.status, "A change applies at once.",
              UI_S_ENTER " change  Esc back", NULL);
}

static void scr_section_wifi(void)   { draw_section(1); }
static void scr_section_fx(void)     { draw_section(3); }
static void scr_section_motion(void) { draw_section(4); }
static void scr_section_system(void) { draw_section(6); }

/* A long list in pages. The header carries the page and its controls,
 * the rail at the side shows where the page sits. */
typedef struct { const char *ssid; int bars; const char *tag; } net_t;

static const net_t MO_NETS[] = {
    { "HomeAP", 3, "saved" }, { "HomeAP-5G", 3, "saved" }, { "garage", 2, "" },
    { "Workshop", 2, "saved" }, { "printer-setup", 2, "open" },
    { "Neighbour 2.4", 1, "" }, { "cafe-guest", 1, "open" }, { "IoT", 3, "" },
    { "vodafone-4F2A", 1, "" }, { "Lab", 2, "" }, { "phone-hotspot", 3, "saved" },
    { "TP-Link_9C10", 1, "" }, { "backup-link", 2, "" },
};

static void draw_list(int lpage, int joined)
{
    static const unsigned STAIR[3] = { 0x2582, 0x2584, 0x2586 };
    ui_slate_t pg = ui_slate_page();
    char note[24];

    ui_focus_default(MO_ROW);
    const ui_rect_t rail = ui_cut_right(&pg.body, 1);
    ui_cut_right(&pg.body, 1);
    const ui_grid_t g = ui_grid(pg.body, pg.m.cls == UI_WIDE ? 3 : 2, 3,
                                pg.m.gutter, 0);
    const int per = ui_grid_capacity(&g);
    const int pages = ui_page_count(NELEM(MO_NETS), per);
    lpage = ui_page_clamp(lpage, NELEM(MO_NETS), per);
    const ui_rect_t head = ui_pages(pg.header, MO_PREV, MO_NEXT, lpage, pages);
    snprintf(note, sizeof note, "%d found", NELEM(MO_NETS));
    ui_title(head, "Wi-Fi", "Networks", note, true);
    ui_rail(rail, lpage, pages);
    for (int i = 0; i < per && lpage * per + i < NELEM(MO_NETS); i++) {
        const int n = lpage * per + i;
        const net_t *w = &MO_NETS[n];
        const bool on = n == joined;
        char ladder[16] = "", right[48];
        for (int b = 0; b < 3; b++)
            ui_utf8_put(ladder + strlen(ladder), b < w->bars ? STAIR[b] : 0x2581);
        const char *tag = on ? UI_S_CHECK " joined" : w->tag;
        if (*tag) snprintf(right, sizeof right, "%s %s", tag, ladder);
        else      snprintf(right, sizeof right, "%s", ladder);
        ui_part(ui_grid_cell(&g, i), (ui_id_t)(MO_ROW + n),
                &(ui_part_t){ .title = w->ssid, .right = right,
                              .tone = on ? UI_TONE_TINT : UI_TONE_CARD,
                              .accent = on ? OVERLAY_COL_GREEN : 0,
                              .flags = on ? UI_PART_RIGHT_BOLD : 0 }, NULL);
    }
    mo_status(pg.status, "Tap a network to join it.",
              UI_S_ENTER " join  PgUp PgDn page  Esc back", NULL);
}

static void scr_list(void)   { draw_list(0, 0); }
static void scr_list_2(void) { draw_list(1, 12); }

/* Access code pad. Keys are three rows high, so a digit sits centered. */
static void draw_unlock(const char *code, bool wrong_try)
{
    static const char *const KEYS[12] = { "1", "2", "3", "4", "5", "6", "7",
                                          "8", "9", UI_S_LEFT, "0", "OK" };
    ui_slate_t pg = ui_slate_page();
    const bool wrong = wrong_try && !*code;
    char shown[64] = "";

    ui_title(pg.header, NULL, "Deck locked", "Enter the access code", false);
    const int kw = 9;
    ui_rect_t pad = ui_center(pg.body, 3 * kw + 2, 15);
    const ui_rect_t entry = ui_cut_top(&pad, 3);
    ui_panel(entry, wrong ? OVERLAY_COL_RED : 0);
    ui_pen(wrong ? OVERLAY_COL_RED : OVERLAY_COL_DEFAULT);
    if (*code) {
        for (const char *c = code; *c; c++)
            strcat(shown, c == code ? UI_S_BULLET : " " UI_S_BULLET);
    }
    ui_text(ui_rect(entry.x + 2, entry.y + 1, entry.w - 4, 1),
            *code ? shown : wrong ? UI_S_CROSS " wrong code" : "access code",
            *code || wrong ? UI_PANEL | UI_BOLD : UI_PANEL_MUTED, UI_CENTER);
    ui_pen(OVERLAY_COL_DEFAULT);
    for (int i = 0; i < 12; i++) {
        const ui_rect_t r = ui_rect(pad.x + (i % 3) * (kw + 1),
                                    pad.y + (i / 3) * 3, kw, 3);
        const uint8_t off = *code ? 0 : UI_PART_OFF;
        if (i == 9)
            ui_push(r, (ui_id_t)(MO_KEY + i), KEYS[i], UI_TONE_CARD, 0, off);
        else if (i == 11)
            ui_push(r, (ui_id_t)(MO_KEY + i), KEYS[i], UI_TONE_SOLID,
                    OVERLAY_COL_GREEN, off);
        else
            ui_push(r, (ui_id_t)(MO_KEY + i), KEYS[i], UI_TONE_CARD, 0, 0);
    }
    mo_status(pg.status, "Tap digits, or type the code.",
              "0-9 type  " UI_S_ENTER " unlock",
              wrong ? "Wrong code. Try again." : NULL);
}

static void scr_unlock(void)       { draw_unlock("", false); }
static void scr_unlock_code(void)  { draw_unlock("12", false); }
static void scr_unlock_wrong(void) { draw_unlock("", true); }

/* Connecting: the steps so far and a way out. The failed form shows the
 * same panel after a refused sign-in. */
static void draw_connect(bool fail)
{
    ui_slate_t pg = ui_slate_page();
    const lab_profile_t *p = &PROFILES[0];
    const where_t w = parse_where(p->where);
    ui_rect_t box = ui_center(pg.area, 46, 11);
    const ui_rect_t card = ui_cut_top(&box, 8);
    char wifi[] = "Wi-Fi  HomeAP", reach[80], head[64], user[64], spin[8];

    snprintf(reach, sizeof reach, "Reach  %s port %s", w.host, w.port);
    if (fail) {
        snprintf(head, sizeof head, "No session on %s", p->name);
        const ui_info_row_t rows[6] = {
            { UI_S_CROSS, head, OVERLAY_COL_WHITE, OVERLAY_COL_RED },
            { "", "" },
            { UI_S_CHECK, wifi, 0, OVERLAY_COL_GREEN },
            { UI_S_CHECK, reach, 0, OVERLAY_COL_GREEN },
            { UI_S_CHECK, "Key exchange", 0, OVERLAY_COL_GREEN },
            { UI_S_CROSS, "Sign in: password refused", OVERLAY_COL_RED,
              OVERLAY_COL_RED },
        };
        ui_info(card, rows, 6, OVERLAY_COL_RED);
        const ui_grid_t g = ui_grid(box, 2, 3, pg.m.gutter, 0);
        ui_push(ui_grid_cell(&g, 0), MO_BTN, "Back", UI_TONE_CARD, 0, 0);
        ui_push(ui_grid_cell(&g, 1), MO_BTN + 1, "Edit host", UI_TONE_SOLID,
                OVERLAY_COL_BLUE, 0);
        mo_status(pg.status, "", UI_S_LEFT UI_S_RIGHT " move  " UI_S_ENTER
                  " choose", "Sign in failed.");
        return;
    }
    ui_utf8_put(spin, spinner_glyph(lab_frame + 2));
    snprintf(head, sizeof head, "Connecting to %s", p->name);
    snprintf(user, sizeof user, "Sign in as %s", w.user);
    const ui_info_row_t rows[6] = {
        { spin, head, OVERLAY_COL_WHITE, OVERLAY_COL_AMBER },
        { "", "" },
        { UI_S_CHECK, wifi, 0, OVERLAY_COL_GREEN },
        { UI_S_CHECK, reach, 0, OVERLAY_COL_GREEN },
        { UI_S_CHEVRON, "Key exchange", OVERLAY_COL_WHITE, OVERLAY_COL_AMBER },
        { UI_S_MIDDOT, user },
    };
    ui_info(card, rows, 6, prof_accent(p->name));
    ui_push(box, MO_BTN, "Cancel", UI_TONE_CARD, 0, 0);
    ui_hot(card, MO_ROW);
    mo_status(pg.status, "Mock: tap the panel to finish.",
              UI_S_ENTER " cancel", NULL);
}

static void scr_connect(void) { draw_connect(false); }
static void scr_fail(void)    { draw_connect(true); }

/* Session menu: a slate sheet at the right over the dimmed terminal. */
static void scr_menu(void)
{
    const ui_metrics_t m = ui_metrics();
    const int cols = ui_cols(), rows = ui_rows();

    ui_dim();
    const int w = cols < (m.cls == UI_COMPACT ? 30 : 36)
                      ? cols : (m.cls == UI_COMPACT ? 30 : 36);
    const ui_rect_t sheet = ui_rect(cols - w, 0, w, rows);
    ui_fill(sheet.x, sheet.y, sheet.w, sheet.h, UI_TEXT);
    ui_rect_t inn = ui_inset(sheet, 1, 0);
    ui_title(ui_cut_top(&inn, 3), "Session", PROFILES[0].name, NULL, true);
    const ui_info_row_t info[2] = { { "Up", "2 h 14 min" },
                                    { "Link", "41 min, -55 dBm" } };
    ui_info(ui_cut_top(&inn, 4), info, 2, 0);
    ui_part(ui_cut_top(&inn, 3), MO_BTN,
            &(ui_part_t){ .title = "Reset terminal" }, NULL);
    ui_part(ui_cut_top(&inn, 3), MO_BTN + 1,
            &(ui_part_t){ .title = "Setup", .right = UI_S_CHEVRON }, NULL);
    ui_push(ui_cut_top(&inn, 3), MO_BTN + 2, "Disconnect", UI_TONE_TINT,
            OVERLAY_COL_RED, 0);
}

static const lab_screen_t MO_SCREENS[] = {
    { "mo-parts",       scr_parts,       .slate = true },
    { "mo-home",        scr_home,        .slate = true },
    { "mo-home-keys",   scr_home,        .slate = true,
      .focus = MO_HOST + 1 },
    { "mo-home-down",   scr_home,        .slate = true,
      .down = MO_HOST + 2 },
    { "mo-host",        scr_host,        .slate = true },
    { "mo-host-key",    scr_host_key,    .slate = true,
      .focus = MO_BTN + 2 },
    { "mo-confirm",     scr_confirm,     .slate = true },
    { "mo-confirm-ask", scr_confirm_ask, .slate = true,
      .focus = MO_BTN + 1 },
    { "mo-edit",        scr_edit,        .slate = true },
    { "mo-edit-new",    scr_edit_new,    .slate = true },
    { "mo-edit-typing", scr_edit_typing, .slate = true },
    { "mo-edit-bad",    scr_edit_bad,    .slate = true },
    { "mo-edit-key",    scr_edit_key,    .slate = true,
      .focus = MO_OPT + 5 },
    { "mo-setup",       scr_setup,       .slate = true,
      .focus = MO_HUB + 3 },
    { "mo-section-wifi",   scr_section_wifi,   .slate = true },
    { "mo-section-fx",     scr_section_fx,     .slate = true },
    { "mo-section-motion", scr_section_motion, .slate = true,
      .down = MO_OPT + 9 },
    { "mo-section-system", scr_section_system, .slate = true,
      .focus = MO_OPT + 16 },
    { "mo-list",        scr_list,        .slate = true },
    { "mo-list-2",      scr_list_2,      .slate = true,
      .focus = MO_ROW + 12 },
    { "mo-unlock",      scr_unlock,      .slate = true },
    { "mo-unlock-code", scr_unlock_code, .slate = true,
      .down = MO_KEY + 4 },
    { "mo-unlock-wrong", scr_unlock_wrong, .slate = true },
    { "mo-connect",     scr_connect,     .slate = true },
    { "mo-fail",        scr_fail,        .slate = true,
      .focus = MO_BTN + 1 },
    { "mo-menu",        scr_menu,        .slate = true, .term = true,
      .focus = MO_BTN },
};

const lab_screen_t *lab_mo_screens(int *n)
{
    *n = NELEM(MO_SCREENS);
    return MO_SCREENS;
}
