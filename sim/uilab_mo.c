/*
 * uilab_mo.c - the lab's slate screens, drawn with the kit's parts.
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
        ui_chips(ui_rect(1, y, half, 3), 31, "Tint", TINT, 2, 0, 0, 0);
        ui_debug_pose(35, UI_POSE_FOCUS);
        ui_debug_pose(36, UI_POSE_DOWN);
        ui_chips(ui_rect(2 + half, y, half, 3), 35, "Tint", TINT, 2, 0, 0, 0);
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
    char name[40], user[40], host[72], port[12], pass[40], keyid[40];
    bool by_key, show;
} form_t;

typedef struct {
    bool is_new;
    form_t f;
    form_t was;               /* the form as loaded: what "changed" means */
    char orig[40];            /* the host name at entry, for the title    */
    int edit;                 /* index of the field in typing, else -1    */
    int caret;                /* caret of that field, -1 = after the last */
    int nkeys;                /* stored keys: the first nkeys of LAB_KEYS */
} edit_t;

#define LONG_HOST "edge-gateway-03.eu-west-2.compute.internal.example-corp.co.uk"

static const struct { const char *id, *type; } LAB_KEYS[2] = {
    { "deck-ed25519", "ssh-ed25519" }, { "work-rsa", "ssh-rsa" },
};

static form_t base_form(void)
{
    return (form_t){ .name = "homelab", .user = "root", .host = "192.168.1.10",
                     .port = "22", .pass = "hunter22" };
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
    f.by_key = strstr(colon, "key") != NULL;
    if (f.by_key) snprintf(f.keyid, sizeof f.keyid, "%s", LAB_KEYS[0].id);
    colon++;
    snprintf(f.port, sizeof f.port, "%.*s", (int)strspn(colon, "0123456789"),
             colon);
    return f;
}

static bool same_form(const form_t *a, const form_t *b)
{
    return !strcmp(a->name, b->name) && !strcmp(a->user, b->user) &&
           !strcmp(a->host, b->host) && !strcmp(a->port, b->port) &&
           !strcmp(a->pass, b->pass) && !strcmp(a->keyid, b->keyid) &&
           a->by_key == b->by_key && a->show == b->show;
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

/* Right text of the Key part, @p cap cells at most. It holds the id, the
 * type and the list place, as far as they fit. */
static void key_text(char *out, size_t sz, const char *key, int nkeys, int cap)
{
    int at = -1;
    for (int i = 0; i < nkeys; i++)
        if (!strcmp(LAB_KEYS[i].id, key)) at = i;
    if (at < 0) {
        if (key[0]) snprintf(out, sz, "%.*s (missing)", cap - 10, key);
        else        snprintf(out, sz, "none stored");
        return;
    }
    const char *id = LAB_KEYS[at].id, *type = LAB_KEYS[at].type;
    char pos[24] = "";
    if (nkeys > 1) snprintf(pos, sizeof pos, " %d/%d", at + 1, nkeys);
    if ((int)(strlen(id) + strlen(type) + strlen(pos)) + 2 <= cap)
        snprintf(out, sz, "%s  %s%s", id, type, pos);
    else
        snprintf(out, sz, "%.*s%s", cap - (int)strlen(pos), id, pos);
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
    const bool user_bad = !f->user[0], key_bad = f->by_key && !f->keyid[0];
    /* An untouched new form shows no red yet. Save stays off until valid. */
    const char *msg_port = port_bad ? "Port: a number from 1 to 65535." : NULL;
    const char *msg_name = name_bad && changed ? "A host needs a name." : NULL;
    const char *msg_host = host_bad && changed ? "A host needs an address." : NULL;
    const char *msg_user = user_bad && changed ? "A host needs a user." : NULL;
    const bool wrong = port_bad || name_bad || host_bad || user_bad || key_bad;
    const char *alert = msg_port ? msg_port : msg_name ? msg_name
                      : msg_host ? msg_host : msg_user ? msg_user
                      : key_bad ? "No stored key. Setup, Hosts imports one."
                                : NULL;

    char title[64];
    if (st->is_new) snprintf(title, sizeof title, "New host");
    else            snprintf(title, sizeof title, "Edit %s", st->orig);
    ui_rect_t head = pg.header;
    ui_pill(&head, MO_PILL, "Save", UI_TONE_SOLID, OVERLAY_COL_GREEN,
            wrong ? UI_PART_OFF : 0);
    ui_title(head, "Hosts", title, NULL, true);

    const int half = (body.w - gx) >> 1, port_w = 17, wide = body.w - gx - port_w;
    const struct { const char *label, *value; int lw; bool mask; const char *bad; }
    fld[5] = {
        { "Name", f->name, 0, false, msg_name },
        { "User", f->user, 0, false, msg_user },
        { "Host", f->host, 0, false, msg_host },
        { "Port", f->port, 6, false, msg_port },
        { f->by_key ? "Passphrase" : "Password", f->pass, f->by_key ? 12 : 0,
          !f->show, NULL },
    };
    const ui_rect_t cell[4] = {
        ui_rect(body.x, body.y, half, 3),
        ui_rect(body.x + half + gx, body.y, body.w - half - gx, 3),
        ui_rect(body.x, body.y + 3, wide, 3),
        ui_rect(body.x + wide + gx, body.y + 3, port_w, 3),
    };
    for (int i = 0; i < 4; i++)
        ui_entry_at(cell[i], (ui_id_t)(MO_FIELD + i), fld[i].label,
                    fld[i].value, fld[i].lw,
                    (uint8_t)((st->edit == i ? UI_ENTRY_EDIT : 0) |
                              (fld[i].bad ? UI_ENTRY_BAD : 0)),
                    st->edit == i && st->caret >= 0 ? st->caret
                                                    : ui_len(fld[i].value));
    int y = body.y + 6;
    ui_chips(ui_rect(body.x, y, body.w, 3), MO_OPT, "Sign-in", AUTH, 2,
             f->by_key ? 1 : 0, 0, 12);
    y += 3;
    if (f->by_key) {
        char right[96];
        key_text(right, sizeof right, f->keyid, st->nkeys, body.w - 12);
        ui_part(ui_rect(body.x, y, ui_act_w("Key", right), 3), MO_OPT + 5,
                &(ui_part_t){ .title = "Key", .right = right,
                              .flags = (uint8_t)(UI_PART_RIGHT_BOLD |
                                  UI_PART_ROOMY |
                                  (st->nkeys ? 0 : UI_PART_OFF)) },
                NULL);
        y += 3;
    }
    const char *toggle = f->by_key ? "Show passphrase" : "Show password";
    ui_entry_at(ui_rect(body.x, y, half, 3), MO_FIELD + 4, fld[4].label,
                fld[4].value, fld[4].lw,
                (uint8_t)((st->edit == 4 ? UI_ENTRY_EDIT : 0) |
                          (fld[4].mask ? UI_ENTRY_MASK : 0)),
                st->edit == 4 && st->caret >= 0 ? st->caret
                                                : ui_len(fld[4].value));
    ui_switch(ui_rect(body.x + half + gx, y, ui_switch_w(toggle), 3),
              MO_OPT + 4, toggle, f->show, UI_PART_ROOMY);
    y += 3;
    if (alert && y < body.y + body.h) {
        ui_pen(OVERLAY_COL_RED);
        ui_text(ui_rect(body.x + 2, y, body.w - 4, 1), alert, UI_TEXT | UI_BOLD,
                UI_LEFT);
        ui_pen(OVERLAY_COL_DEFAULT);
    }
    if (st->edit >= 0)
        mo_status(pg.status, "Type on the keyboard.",
                  "type  " UI_S_ENTER " done  " UI_S_UP UI_S_DOWN " leave",
                  NULL);
    else
        mo_status(pg.status, "Tap a field to change it.",
                  UI_S_ENTER " change  Esc cancel", NULL);
}

static edit_t edit_state(int n)
{
    edit_t e = { .edit = -1, .caret = -1, .nkeys = 2 };
    e.f = e.was = form_of(&PROFILES[0]);
    snprintf(e.orig, sizeof e.orig, "%s", PROFILES[0].name);
    switch (n) {
    case 1:                                    /* a new, empty form      */
        e.is_new = true;
        e.orig[0] = 0;
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
    case 4:                                    /* key sign-in, two keys  */
        e.f = e.was = base_form();
        e.f.by_key = true;
        snprintf(e.f.keyid, sizeof e.f.keyid, "%s", LAB_KEYS[0].id);
        break;
    case 5:                                    /* key sign-in, no key    */
        e.f = e.was = base_form();
        e.f.by_key = true;
        e.nkeys = 0;
        break;
    case 6:                                    /* caret in a long host   */
        e.f = e.was = base_form();
        snprintf(e.f.host, sizeof e.f.host, "%s", LONG_HOST);
        e.edit = 2;
        e.caret = 24;
        break;
    case 7:                                    /* caret in the secret    */
        e.f = e.was = base_form();
        e.edit = 4;
        e.caret = 3;
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
static void scr_edit_nokey(void)  { draw_edit_n(5); }
static void scr_edit_caret(void)  { draw_edit_n(6); }
static void scr_edit_secret(void) { draw_edit_n(7); }

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
    const ui_grid_t g = ui_grid(pg.body,
                                ui_cols_for(pg.body.w, 30, pg.m.gutter), 4,
                                pg.m.gutter, 0);
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
    ui_flow_t f = ui_flow(pg.body, pg.m.gutter, 3);
#define BTN(n, label, tone, accent) \
    ui_push(ui_flow_next(&f, ui_push_w(label)), MO_BTN + (n), label, tone, \
            accent, 0)
    BTN(0, "Connect", UI_TONE_SOLID, OVERLAY_COL_GREEN);
    BTN(1, "Edit", UI_TONE_CARD, 0);
    BTN(2, "Delete", UI_TONE_TINT, OVERLAY_COL_RED);
    ui_flow_break(&f);
    BTN(3, UI_S_UP " Move earlier", UI_TONE_CARD, 0);
    BTN(4, UI_S_DOWN " Move later", UI_TONE_CARD, 0);
#undef BTN
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
    ui_flow_t f = ui_flow(box, pg.m.gutter, 3);
    ui_push(ui_flow_next(&f, ui_push_w("Cancel")), MO_BTN, "Cancel",
            UI_TONE_CARD, 0, 0);
    ui_push(ui_flow_next(&f, ui_push_w(ask->verb)), MO_BTN + 1, ask->verb,
            UI_TONE_SOLID, ask->accent, 0);
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
    const ui_grid_t g = ui_grid(pg.body,
                                ui_cols_for(pg.body.w, 24, pg.m.gutter), 4,
                                pg.m.gutter, 0);
    for (int i = 0; i < NELEM(MO_SETUP); i++)
        ui_part(ui_grid_cell(&g, i), (ui_id_t)(MO_HUB + i),
                &(ui_part_t){ .title = MO_SETUP[i].name,
                              .detail = MO_SETUP[i].note,
                              .right = UI_S_CHEVRON }, NULL);
    mo_status(pg.status, "The bar at the top goes back.",
              UI_S_ENTER " open  Esc back", NULL);
}

typedef struct { int n; struct { int it, w; } c[4]; } sec_row_t;

static int own_w(const item_t *it)
{
    return it->type == IT_TOGGLE ? ui_switch_w(it->label)
                                 : ui_act_w(it->label,
                                            it->nav ? UI_S_CHEVRON : "");
}

/* Pack the items into rows of @p room cells. A choice takes a row and
 * the rest keep their own width. */
static int pack_rows(const cat_t *c, int room, int gx, sec_row_t *rows)
{
    int nrows = 0, used = 0;
    bool open = false;

    for (int n = 0; n < c->n; n++) {
        const item_t *it = &c->it[n];
        const int own = own_w(it);
        const int w = it->type == IT_PICK ? room : own < room ? own : room;
        if (it->type == IT_PICK) {
            rows[nrows].n = 1;
            rows[nrows].c[0].it = n;
            rows[nrows++].c[0].w = w;
            open = false;
        } else if (open && used + gx + w <= room) {
            sec_row_t *r = &rows[nrows - 1];
            r->c[r->n].it = n;
            r->c[r->n++].w = w;
            used += gx + w;
        } else {
            rows[nrows].n = 1;
            rows[nrows].c[0].it = n;
            rows[nrows++].c[0].w = w;
            open = true;
            used = w;
        }
    }
    return nrows;
}

/* One section. A choice takes a row and shows every option. Switches and
 * actions keep their own width and pack from the left. */
static void draw_section(int cat)
{
    ui_slate_t pg = ui_slate_page();
    const cat_t *c = &MO_SETUP[cat];
    sec_row_t rows[4];
    const int gx0 = pg.m.gutter;

    ui_focus_default(MO_OPT);
    const int per = pg.body.h / 3 > 1 ? pg.body.h / 3 : 1;
    int nrows = pack_rows(c, pg.body.w, gx0, rows);
    if (nrows > per) nrows = pack_rows(c, pg.body.w - 2, gx0, rows);
    int cap_w = 0;
    for (int n = 0; n < c->n; n++)
        if (c->it[n].type == IT_PICK) {
            const int w = ui_caption_w(c->it[n].label);
            if (w > cap_w) cap_w = w;
        }
    const int pages = ui_page_count(nrows, per);
    const ui_rect_t head = ui_pages(pg.header, MO_PREV, MO_NEXT, 0, pages);
    ui_title(head, "Setup", c->name, NULL, true);
    if (pages > 1) {
        ui_rail(ui_cut_right(&pg.body, 1), 0, pages);
        ui_cut_right(&pg.body, 1);
    }
    const ui_rect_t body = pg.body;
    const int gx = pg.m.gutter;
    for (int ri = 0; ri < per && ri < nrows; ri++) {
        const int y = body.y + ri * 3;
        int x = body.x;
        for (int ci = 0; ci < rows[ri].n; ci++) {
            const int n = rows[ri].c[ci].it;
            const item_t *it = &c->it[n];
            const ui_id_t id = (ui_id_t)(MO_OPT + n * 8);
            const ui_rect_t r = ui_rect(x, y, rows[ri].c[ci].w, 3);
            x += r.w + gx;
            if (it->type == IT_PICK)
                ui_chips(r, id, it->label, it->set, it->nset, it->init, 0,
                         cap_w);
            else if (it->type == IT_TOGGLE)
                ui_switch(r, id, it->label, it->init != 0, UI_PART_ROOMY);
            else
                ui_part(r, id,
                        &(ui_part_t){ .title = it->label,
                                      .right = it->nav ? UI_S_CHEVRON : NULL,
                                      .tone = it->accent ? UI_TONE_TINT
                                                         : UI_TONE_CARD,
                                      .accent = it->accent,
                                      .flags = UI_PART_ROOMY }, NULL);
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

/* The pad in a known state. A flow has a back target; the device gate
 * has none. */
enum { UP_UNLOCK, UP_OLD, UP_NEW, UP_CONFIRM, UP_REMOVE };

typedef struct {
    const char *code;         /* the typed characters                      */
    int         expected;     /* known code length, 0 when unknown         */
    int         phase;        /* UP_*                                      */
    bool        creating, flow, resume, deriving;
    const char *note;         /* a red line, or NULL                       */
    int         wait;         /* seconds left of a backoff                 */
    char        reveal;       /* the newest character echoed, 0 for none  */
} unlock_t;

static void unlock_head(const unlock_t *u, const char **title,
                        const char **note)
{
    switch (u->phase) {
    case UP_OLD:
        *title = "Change code";
        *note  = "Enter the current code";
        break;
    case UP_NEW:
    case UP_CONFIRM:
        *title = u->creating ? "Create keystore" : "Change code";
        *note  = u->phase == UP_NEW ? "Choose a new code, 4 or more"
                                    : "Enter the new code again";
        break;
    case UP_REMOVE:
        *title = "Remove code";
        *note  = "Enter the code to remove it";
        break;
    default:
        *title = u->flow ? "Keystore locked" : "Deck locked";
        *note  = u->resume ? "Enter the access code to connect"
                           : "Enter the access code";
        break;
    }
}

/* Bullets for the taken characters, muted dots for those still to come. */
static void draw_code_row(ui_rect_t line, const unlock_t *u, int len)
{
    const ui_code_t c = ui_code_plan(len, u->expected, line.w);
    const int step = c.spaced ? 2 : 1;
    int x = line.x + (line.w - c.width) / 2;

    ui_pen(OVERLAY_COL_DEFAULT);
    for (int i = 0; i < c.bullets + c.dots; i++, x += step) {
        if (i >= c.bullets)
            ui_putch(x, line.y, 0xB7, UI_PANEL_MUTED);
        else if (i == c.bullets - 1 && u->reveal)
            ui_putch(x, line.y, (uint16_t)(uint8_t)u->reveal,
                     UI_PANEL | UI_BOLD);
        else
            ui_putch(x, line.y, 0x2022, UI_PANEL | UI_BOLD);
    }
}

static void draw_entry_panel(ui_rect_t entry, const unlock_t *u, int len)
{
    const ui_rect_t line = ui_rect(entry.x + 2, entry.y + 1, entry.w - 4, 1);
    char msg[64];

    ui_panel(entry, u->note && !u->deriving ? OVERLAY_COL_RED : 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    if (u->deriving) {
        char glyph[8];
        ui_utf8_put(glyph, spinner_glyph(lab_frame));
        snprintf(msg, sizeof msg, "%s Checking", glyph);
        ui_text(line, msg, UI_PANEL | UI_BOLD, UI_CENTER);
    } else if (u->note) {
        if (u->wait > 0 && !len)
            snprintf(msg, sizeof msg, "%s, wait %d s", u->note, u->wait);
        else
            snprintf(msg, sizeof msg, "%s", u->note);
        ui_pen(OVERLAY_COL_RED);
        ui_text(line, msg, UI_PANEL | UI_BOLD, UI_CENTER);
    } else if (u->wait > 0 && !len) {
        snprintf(msg, sizeof msg, "Wait %d s", u->wait);
        ui_pen(OVERLAY_COL_AMBER);
        ui_text(line, msg, UI_PANEL | UI_BOLD, UI_CENTER);
    } else if (!len) {
        ui_text(line, u->phase == UP_NEW || u->phase == UP_CONFIRM
                          ? "new code" : "access code",
                UI_PANEL_MUTED, UI_CENTER);
    } else {
        draw_code_row(line, u, len);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

/* Access code pad. Keys are three rows high, so a digit sits centered. */
static void draw_unlock(const unlock_t *u)
{
    static const char *const KEYS[12] = { "1", "2", "3", "4", "5", "6", "7",
                                          "8", "9", UI_S_LEFT, "0", "OK" };
    ui_slate_t pg = ui_slate_page();
    const int len = (int)strlen(u->code);
    const char *title, *note;

    unlock_head(u, &title, &note);
    ui_title(pg.header, NULL, title, note, u->flow);
    const int kw = 9;
    ui_rect_t pad = ui_center(pg.body, 3 * kw + 2, 15);
    draw_entry_panel(ui_cut_top(&pad, 3), u, len);
    for (int i = 0; i < 12; i++) {
        const ui_rect_t r = ui_rect(pad.x + (i % 3) * (kw + 1),
                                    pad.y + (i / 3) * 3, kw, 3);
        const uint8_t off = len ? 0 : UI_PART_OFF;
        if (i == 9)
            ui_push(r, (ui_id_t)(MO_KEY + i), KEYS[i], UI_TONE_CARD, 0, off);
        else if (i == 11)
            ui_push(r, (ui_id_t)(MO_KEY + i), KEYS[i], UI_TONE_SOLID,
                    OVERLAY_COL_GREEN, off);
        else
            ui_push(r, (ui_id_t)(MO_KEY + i), KEYS[i], UI_TONE_CARD, 0, 0);
    }
    mo_status(pg.status, "Tap digits, or type the code.",
              "0-9 type  " UI_S_ENTER " unlock", NULL);
}

static void scr_unlock(void)       { draw_unlock(&(unlock_t){ "" }); }
static void scr_unlock_code(void)  { draw_unlock(&(unlock_t){ "12" }); }
static void scr_unlock_known(void)
{
    draw_unlock(&(unlock_t){ "12", .expected = 6 });
}
static void scr_unlock_wrong(void)
{
    draw_unlock(&(unlock_t){ "", .note = UI_S_CROSS " wrong code", .wait = 12 });
}
static void scr_unlock_wait(void)  { draw_unlock(&(unlock_t){ "", .wait = 12 }); }
static void scr_unlock_check(void)
{
    draw_unlock(&(unlock_t){ "", .deriving = true });
}
static void scr_unlock_new(void)
{
    draw_unlock(&(unlock_t){ "ab7", .phase = UP_NEW, .creating = true,
                             .flow = true, .reveal = '7' });
}
static void scr_unlock_mismatch(void)
{
    draw_unlock(&(unlock_t){ "", .phase = UP_NEW, .flow = true,
                             .note = "The codes do not match" });
}
static void scr_unlock_resume(void)
{
    draw_unlock(&(unlock_t){ "1", .expected = 4, .flow = true,
                             .resume = true });
}

/* Connecting: what the shell knows, and a way out. @p retry shows the
 * wait before the next try. The failed form is a red panel after a
 * refused sign-in; @p lng gives it a reason that wraps. */
static void draw_connect(bool fail, bool retry, bool lng)
{
    ui_slate_t pg = ui_slate_page();
    const lab_profile_t *p = &PROFILES[0];
    const where_t w = parse_where(p->where);
    char head[64], addr[96];

    snprintf(addr, sizeof addr, "%s@%s port %s", w.user, w.host, w.port);
    if (fail) {
        const char *reason = lng ? "the server offered no method the deck supports"
                                 : "wrong password";
        char refused[128], l1[128], l2[128];
        snprintf(refused, sizeof refused, "Sign in refused: %s", reason);
        /* The value column: the panel less 4, less the label column and its gap. */
        const int nl = ui_wrap2(refused, 46 - 4 - 3, l1, l2, sizeof l1);
        snprintf(head, sizeof head, "No session on %s", p->name);
        ui_info_row_t rows[5] = {
            { UI_S_CROSS, head, OVERLAY_COL_WHITE, OVERLAY_COL_RED },
            { "", "" },
            { "", l1 },
        };
        int n = 3;
        if (nl == 2) rows[n++] = (ui_info_row_t){ "", l2 };
        rows[n++] = (ui_info_row_t){ "", addr };
        ui_rect_t box = ui_center(pg.area, 46, n + 5);
        const ui_rect_t card = ui_cut_top(&box, n + 2);
        ui_info(card, rows, n, OVERLAY_COL_RED);
        ui_focus_default(MO_BTN + 1);
        ui_flow_t f = ui_flow(box, pg.m.gutter, 3);
        ui_push(ui_flow_next(&f, ui_push_w("Back")), MO_BTN, "Back",
                UI_TONE_CARD, 0, 0);
        ui_push(ui_flow_next(&f, ui_push_w("Try again")), MO_BTN + 1,
                "Try again", UI_TONE_CARD, 0, 0);
        ui_push(ui_flow_next(&f, ui_push_w("Edit host")), MO_BTN + 2,
                "Edit host", UI_TONE_SOLID, OVERLAY_COL_BLUE, 0);
        mo_status(pg.status, "", UI_S_LEFT UI_S_RIGHT " move  " UI_S_ENTER
                  " choose", NULL);
        return;
    }
    ui_rect_t box = ui_center(pg.area, 46, 10);
    const ui_rect_t card = ui_cut_top(&box, 7);
    char spin[8];
    ui_utf8_put(spin, spinner_glyph(lab_frame + 2));
    snprintf(head, sizeof head, "%s %s", retry ? "Retrying" : "Connecting to",
             p->name);
    const ui_info_row_t rows[5] = {
        { spin, head, OVERLAY_COL_WHITE, OVERLAY_COL_AMBER },
        { "", "" },
        { UI_S_CHECK, "Wi-Fi  HomeAP", 0, OVERLAY_COL_GREEN },
        { "", addr },
        { "", retry ? "Next try in 3 s" : "Try 2, 14 s" },
    };
    ui_info(card, rows, 5, prof_accent(p->name));
    ui_push(ui_rect(box.x, box.y, ui_push_w("Cancel"), 3), MO_BTN, "Cancel",
            UI_TONE_CARD, 0, 0);
    ui_hot(card, MO_ROW);
    mo_status(pg.status, "Mock: tap the panel to finish.",
              UI_S_ENTER " cancel", NULL);
}

static void scr_connect(void)       { draw_connect(false, false, false); }
static void scr_connect_retry(void) { draw_connect(false, true, false); }
static void scr_fail(void)          { draw_connect(true, false, false); }
static void scr_fail_long(void)     { draw_connect(true, false, true); }

/* The host-key pages: a text panel, the fingerprint panel and one row of
 * buttons under a back header. A changed key has a red stripe, and
 * Cancel holds the first focus. */
#define HK_FP "3f9a1c7e5b02d84a6c19e7f30b5d28a41e6c7f902ab3d5c8e1f4067a9b2d3c58"

static void draw_hostkey(bool changed)
{
    ui_slate_t pg = ui_slate_page();
    const lab_profile_t *p = &PROFILES[0];
    const where_t w = parse_where(p->where);
    char l[3][96];
    int n;

    ui_title(pg.header, NULL, changed ? "Host key changed" : "New host key",
             NULL, true);
    if (changed) {
        snprintf(l[0], sizeof l[0], "The key of %s differs from the pinned one.",
                 w.host);
        snprintf(l[1], sizeof l[1],
                 "Replace it only if you know the server got a new key.");
        n = 2;
    } else {
        snprintf(l[0], sizeof l[0], "First connection to %s,", p->name);
        snprintf(l[1], sizeof l[1], "%s port %s.", w.host, w.port);
        snprintf(l[2], sizeof l[2], "Compare the fingerprint before you trust it.");
        n = 3;
    }
    const ui_rect_t card = ui_cut_top(&pg.body, n + 2);
    ui_panel(card, changed ? OVERLAY_COL_RED : 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    for (int i = 0; i < n; i++)
        ui_text(ui_rect(card.x + 2, card.y + 1 + i, card.w - 4, 1), l[i],
                i == n - 1 ? UI_PANEL_MUTED : UI_PANEL, UI_LEFT);
    const ui_rect_t fp = ui_cut_top(&pg.body, 5);
    ui_panel(fp, 0);
    ui_pen(OVERLAY_COL_CYAN);
    ui_text(ui_rect(fp.x + 2, fp.y + 1, fp.w - 4, 1), "SHA256", UI_PANEL_MUTED,
            UI_LEFT);
    ui_pen(OVERLAY_COL_WHITE);
    for (int row = 0; row < 2; row++) {
        char groups[40] = "";
        for (int g = 0; g < 8; g++)
            snprintf(groups + strlen(groups), sizeof groups - strlen(groups),
                     "%s%.4s", g ? " " : "", HK_FP + row * 32 + g * 4);
        ui_text(ui_rect(fp.x + 2, fp.y + 2 + row, fp.w - 4, 1), groups,
                UI_PANEL | UI_BOLD, UI_LEFT);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_flow_t f = ui_flow(pg.body, pg.m.gutter, 3);
    if (changed) {
        ui_push(ui_flow_next(&f, ui_push_w("Cancel")), MO_BTN, "Cancel",
                UI_TONE_CARD, 0, 0);
        ui_push(ui_flow_next(&f, ui_push_w("Replace key")), MO_BTN + 1,
                "Replace key", UI_TONE_TINT, OVERLAY_COL_RED, 0);
    } else {
        ui_push(ui_flow_next(&f, ui_push_w("Trust and connect")), MO_BTN,
                "Trust and connect", UI_TONE_SOLID, OVERLAY_COL_GREEN, 0);
        ui_push(ui_flow_next(&f, ui_push_w("Cancel")), MO_BTN + 1, "Cancel",
                UI_TONE_CARD, 0, 0);
    }
    mo_status(pg.status, "The bar at the top goes back.",
              UI_S_LEFT UI_S_RIGHT " move  " UI_S_ENTER " choose  Esc cancel",
              NULL);
}

static void scr_hostkey(void)         { draw_hostkey(false); }
static void scr_hostkey_changed(void) { draw_hostkey(true); }

static void scr_confirm_replace(void)
{
    draw_confirm(&(ask_t){ "Replace the pinned key?", "Replace",
                           { "A changed key can mean an attack.",
                             "The old pin is lost." },
                           OVERLAY_COL_RED });
}

/* Pair a keyboard: @p n found, @p left seconds before the scan closes
 * (0: no countdown). Forget bonds sits at the bottom, so it stays put
 * while the list changes. */
static const struct { const char *name; int random; } MO_KBDS[8] = {
    { "Keychron K2", 0 }, { "Logitech K380", 1 }, { "MX Keys Mini", 1 },
    { "Apple Magic Keyboard", 0 }, { "Kinesis Adv360", 1 },
    { "ZSA Moonlander", 0 }, { "HHKB Hybrid", 1 }, { "Ducky One 3", 0 },
};

static void draw_pair(int n, int left, int page)
{
    ui_slate_t pg = ui_slate_page();
    const ui_rect_t body = pg.body;
    char spin[8];

    ui_grid_t g = ui_grid(ui_rect(body.x, body.y + 1, body.w, body.h - 5),
                          ui_cols_for(body.w, 30, pg.m.gutter), 4,
                          pg.m.gutter, 0);
    const int per = ui_grid_capacity(&g);
    const int pages = ui_page_count(n, per);
    page = ui_page_clamp(page, n, per);
    const ui_rect_t head = ui_pages(pg.header, MO_PREV, MO_NEXT, page, pages);
    ui_title(head, "Keyboard", "Pair keyboard", NULL, true);
    const int first = page * per;
    const int shown = n - first < per ? n - first : per;
    ui_focus_default(shown > 0 ? (ui_id_t)(MO_HOST + first) : MO_BTN);
    char text[32];
    snprintf(text, sizeof text, "%d found. Tap yours.", n);
    ui_utf8_put(spin, spinner_glyph(lab_frame + 2));
    ui_state(ui_rect(body.x, body.y, body.w, 1), n ? UI_S_CHECK : spin,
             n ? OVERLAY_COL_GREEN : OVERLAY_COL_AMBER,
             n ? text : "Scanning for keyboards");
    if (left) {
        char t[24];
        snprintf(t, sizeof t, "closes in %d s", left);
        ui_pen(OVERLAY_COL_AMBER);
        ui_text(ui_rect(body.x, body.y, body.w - 2, 1), t, UI_TEXT, UI_RIGHT);
        ui_pen(OVERLAY_COL_DEFAULT);
    }
    for (int i = 0; i < shown; i++)
        ui_part(ui_grid_cell(&g, i), (ui_id_t)(MO_HOST + first + i),
                &(ui_part_t){ .title = MO_KBDS[first + i].name,
                              .detail = MO_KBDS[first + i].random
                                            ? "random address"
                                            : "public address",
                              .stripe = OVERLAY_COL_GREEN }, NULL);
    ui_text(ui_rect(body.x + 2, body.y + body.h - 4, body.w - 2, 1),
            "Put the keyboard in pairing mode.", UI_MUTED, UI_LEFT);
    ui_push(ui_rect(body.x, body.y + body.h - 3, ui_push_w("Forget bonds"), 3),
            MO_BTN, "Forget bonds", UI_TONE_TINT, OVERLAY_COL_RED, 0);
    mo_status(pg.status, "", "", NULL);
}

static void scr_pair_scan(void)    { draw_pair(0, 0, 0); }
static void scr_pair(void)         { draw_pair(2, 0, 0); }
static void scr_pair_closing(void) { draw_pair(1, 9, 0); }
static void scr_pair_many(void)    { draw_pair(8, 0, 0); }
static void scr_pair_many_2(void)  { draw_pair(8, 0, 1); }

static void scr_confirm_forget(void)
{
    draw_confirm(&(ask_t){ "Forget every paired keyboard?", "Forget",
                           { "Every bond is deleted from the deck.",
                             "Pair again from this page." },
                           OVERLAY_COL_RED });
}

/* Fake codes for the pages below: finder squares, timing lines and
 * hashed modules, as the sim's stubs draw them. Not scannable. */
static bool qr_fake(int x, int y, int size, uint32_t seed)
{
    if (x < 0 || y < 0 || x >= size || y >= size) return false;
    if ((x < 8 && y < 8) || (x >= size - 8 && y < 8) ||
        (x < 8 && y >= size - 8)) {
        for (int c = 0; c < 3; c++) {
            const int dx = x - (c == 1 ? size - 7 : 0);
            const int dy = y - (c == 2 ? size - 7 : 0);
            if (dx < 0 || dx > 6 || dy < 0 || dy > 6) continue;
            return dx == 0 || dx == 6 || dy == 0 || dy == 6 ||
                   (dx >= 2 && dx <= 4 && dy >= 2 && dy <= 4);
        }
        return false;
    }
    if (x == 6 || y == 6) return ((x + y) & 1) == 0;
    uint32_t h = (((uint32_t)x * 31u + (uint32_t)y) ^ seed) * 2654435761u;
    h ^= h >> 15;
    return (h >> 3) & 1;
}

static bool qr_wifi(int x, int y) { return qr_fake(x, y, 33, 0x5EED0001u); }
static bool qr_host(int x, int y) { return qr_fake(x, y, 25, 0x5EED0002u); }

/* Add a network from the phone, in one of four phases. */
enum { PH_WAIT, PH_TEST, PH_FAIL, PH_OK };

static void draw_wifi(int phase)
{
    ui_slate_t pg = ui_slate_page();
    const ui_step_t steps[4] = {
        { .mark = '1', .text = "Get the \"ESP SoftAP Provisioning\" app" },
        { .mark = '2', .text = "Join this Wi-Fi:", .value = "CYBERDECK-SIM",
          .pen = OVERLAY_COL_GREEN },
        { .mark = '3', .text = "Proof code:", .value = "1A2B3C4D",
          .pen = OVERLAY_COL_AMBER },
        { .mark = '4', .text = "Pick your Wi-Fi and password in the app." },
    };
    char spin[8];
    ui_utf8_put(spin, spinner_glyph(lab_frame + 2));
    ui_onboard_t o = { .steps = steps, .nsteps = 4, .qr_size = 33,
                       .qr_mod = qr_wifi, .qr_caption = "scan with the app" };

    ui_title(pg.header, "Wi-Fi", "Add network (phone)", NULL, true);
    switch (phase) {
    case PH_WAIT:
        o.mark = spin; o.pen = OVERLAY_COL_GREEN;
        o.state = "Waiting for the phone";
        break;
    case PH_TEST:
        o.mark = spin; o.pen = OVERLAY_COL_AMBER;
        o.state = "Testing the network";
        break;
    case PH_FAIL:
        o.mark = UI_S_CROSS; o.pen = OVERLAY_COL_RED;
        o.state = "Wrong password, or the network was not found.";
        o.state2 = "Try again from the app. Back cancels.";
        break;
    default:
        o.nsteps = 0; o.qr_size = 0; o.mark = UI_S_CHECK;
        o.pen = OVERLAY_COL_GREEN;
        o.state = "Joined HomeAP. Saved.";
        o.state2 = "Back to HOME in 2 s";
        break;
    }
    ui_onboard(pg.body, &o);
    mo_status(pg.status, "", "", NULL);
}

static void scr_wifi(void)      { draw_wifi(PH_WAIT); }
static void scr_wifi_test(void) { draw_wifi(PH_TEST); }
static void scr_wifi_fail(void) { draw_wifi(PH_FAIL); }
static void scr_wifi_ok(void)   { draw_wifi(PH_OK); }

/* Import hosts from the phone (the deck's own Wi-Fi) or from a PC. */
typedef struct {
    bool        pc, pop_off;
    int         saved, removed;
    const char *name, *err;
} import_t;

static void draw_import(const import_t *im)
{
    ui_slate_t pg = ui_slate_page();
    const ui_step_t pc_steps[4] = {
        { .mark = '1', .text = "Open on your PC:",
          .value = "http://192.168.1.158", .pen = OVERLAY_COL_WHITE },
        im->pop_off
            ? (ui_step_t){ .mark = '2', .pen = OVERLAY_COL_AMBER,
                           .text = "Proof code is off in this build. "
                                   "The page is open." }
            : (ui_step_t){ .mark = '2', .text = "Proof code:",
                           .value = "1A2B3C4D", .pen = OVERLAY_COL_AMBER },
        { .mark = '3', .text = "Fill the form and save." },
        { .mark = '!', .text = "Plain HTTP. Use it on a network you trust.",
          .pen = OVERLAY_COL_AMBER },
    };
    const ui_step_t phone_steps[4] = {
        { .mark = '1', .text = "Join this Wi-Fi:", .value = "DECK-SETUP-SIM",
          .pen = OVERLAY_COL_GREEN },
        { .mark = '2', .text = "Wi-Fi password:", .value = "1A2B3C4D",
          .pen = OVERLAY_COL_AMBER },
        { .mark = '3', .text = "Open in a browser:",
          .value = "http://192.168.4.1", .pen = OVERLAY_COL_WHITE },
        { .mark = '4', .text = "Fill the form and save. Repeat for more." },
    };
    char spin[8], state[96];
    ui_utf8_put(spin, spinner_glyph(lab_frame + 2));
    ui_onboard_t o = { .steps = im->pc ? pc_steps : phone_steps, .nsteps = 4,
                       .qr_size = 25, .qr_mod = qr_host,
                       .qr_caption = im->pc ? "scan to open" : "scan to join",
                       .state = state };

    ui_title(pg.header, "Hosts",
             im->pc ? "Import from PC" : "Import from phone", NULL, true);
    o.mark = UI_S_CHECK;
    o.pen = OVERLAY_COL_GREEN;
    if (im->err) {
        o.mark = UI_S_CROSS;
        o.pen = OVERLAY_COL_RED;
        snprintf(state, sizeof state, "Rejected: %s", im->err);
    } else if (im->saved && im->removed) {
        snprintf(state, sizeof state, "Last: %s (%d saved, %d removed)",
                 im->name, im->saved, im->removed);
    } else if (im->removed) {
        snprintf(state, sizeof state, "Removed %s (%d removed)", im->name,
                 im->removed);
    } else if (im->saved) {
        snprintf(state, sizeof state, "Imported %s (%d saved)", im->name,
                 im->saved);
    } else {
        o.mark = spin;
        snprintf(state, sizeof state, "Waiting for a browser");
    }
    ui_onboard(pg.body, &o);
    mo_status(pg.status, "", "", NULL);
}

static void scr_import_phone(void)  { draw_import(&(import_t){ 0 }); }
static void scr_import_pc(void)     { draw_import(&(import_t){ .pc = true }); }
static void scr_import_pc_off(void)
{
    draw_import(&(import_t){ .pc = true, .pop_off = true });
}
static void scr_import_saved(void)
{
    draw_import(&(import_t){ .pc = true, .saved = 2, .name = "nas" });
}
static void scr_import_both(void)
{
    draw_import(&(import_t){ .saved = 2, .removed = 1, .name = "homelab" });
}
static void scr_import_removed(void)
{
    draw_import(&(import_t){ .removed = 1, .name = "old-box" });
}
static void scr_import_rejected(void)
{
    draw_import(&(import_t){ .pc = true, .err = "a host needs an address" });
}

/* Session menu: a slate sheet at the right over the dimmed terminal. */
static void scr_menu(void)
{
    const int cols = ui_cols(), rows = ui_rows();

    ui_dim();
    const int w = cols < 28 ? cols : 28;
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
    { "mo-confirm-replace", scr_confirm_replace, .slate = true,
      .focus = MO_BTN },
    { "mo-edit",        scr_edit,        .slate = true },
    { "mo-edit-new",    scr_edit_new,    .slate = true },
    { "mo-edit-typing", scr_edit_typing, .slate = true },
    { "mo-edit-bad",    scr_edit_bad,    .slate = true },
    { "mo-edit-key",    scr_edit_key,    .slate = true,
      .focus = MO_OPT + 5 },
    { "mo-edit-nokey",  scr_edit_nokey,  .slate = true },
    { "mo-edit-caret",  scr_edit_caret,  .slate = true },
    { "mo-edit-secret", scr_edit_secret, .slate = true },
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
    { "mo-unlock-known", scr_unlock_known, .slate = true },
    { "mo-unlock-wrong", scr_unlock_wrong, .slate = true },
    { "mo-unlock-wait", scr_unlock_wait, .slate = true },
    { "mo-unlock-check", scr_unlock_check, .slate = true },
    { "mo-unlock-new",  scr_unlock_new,  .slate = true },
    { "mo-unlock-mismatch", scr_unlock_mismatch, .slate = true },
    { "mo-unlock-resume", scr_unlock_resume, .slate = true },
    { "mo-connect",     scr_connect,     .slate = true },
    { "mo-connect-retry", scr_connect_retry, .slate = true },
    { "mo-fail",        scr_fail,        .slate = true,
      .focus = MO_BTN + 1 },
    { "mo-fail-long",   scr_fail_long,   .slate = true,
      .focus = MO_BTN + 1 },
    { "mo-hostkey",     scr_hostkey,     .slate = true,
      .focus = MO_BTN },
    { "mo-hostkey-changed", scr_hostkey_changed, .slate = true,
      .focus = MO_BTN },
    { "mo-menu",        scr_menu,        .slate = true, .term = true,
      .focus = MO_BTN },
    { "mo-pair-scan",   scr_pair_scan,   .slate = true },
    { "mo-pair",        scr_pair,        .slate = true,
      .focus = MO_HOST },
    { "mo-pair-closing", scr_pair_closing, .slate = true,
      .focus = MO_BTN },
    { "mo-pair-many",   scr_pair_many,   .slate = true,
      .down = MO_HOST + 1 },
    { "mo-pair-many-2", scr_pair_many_2, .slate = true,
      .focus = MO_HOST + 6 },
    { "mo-confirm-forget", scr_confirm_forget, .slate = true,
      .focus = MO_BTN },
    { "mo-wifiprov",    scr_wifi,        .slate = true },
    { "mo-wifiprov-test", scr_wifi_test, .slate = true },
    { "mo-wifiprov-fail", scr_wifi_fail, .slate = true },
    { "mo-wifiprov-ok", scr_wifi_ok,     .slate = true },
    { "mo-import-phone", scr_import_phone, .slate = true },
    { "mo-import-pc",   scr_import_pc,   .slate = true },
    { "mo-import-pc-off", scr_import_pc_off, .slate = true },
    { "mo-import-saved", scr_import_saved, .slate = true },
    { "mo-import-both", scr_import_both, .slate = true },
    { "mo-import-removed", scr_import_removed, .slate = true },
    { "mo-import-rejected", scr_import_rejected, .slate = true },
};

const lab_screen_t *lab_mo_screens(int *n)
{
    *n = NELEM(MO_SCREENS);
    return MO_SCREENS;
}
