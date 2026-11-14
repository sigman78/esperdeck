/*
 * uilab.c — the UI kit's gallery: mock screens composed only from kit
 * calls, rendered through the real display path. Headless, it writes one
 * BMP per screen (--out DIR); with a window, the mouse is the finger and
 * the arrow keys drive focus. Host-only tool; nothing here ships.
 *
 *   cyberdeck_uilab [--font 8x16|10x20|12x24] [--out DIR] [--dump DIR]
 *                   [--screen NAME]
 *
 * --dump writes the font's glyphs, the baked style tables and every
 * posed frame's cells as JSON: the input of tools/uimock.
 */

#include "uilab.h"
#include "display.h"
#include "display_fx.h"
#include "font.h"
#include "ui_frame.h"

#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* Region ids. Each screen owns a base; list rows add their index. */
enum {
    ID_PROFILE = 100, ID_NEW = 150, ID_LOCK, ID_SETTINGS,
    ID_HUB = 200, ID_VALUE = 300, ID_ROW = 400,
    ID_FIELD = 500, ID_AUTH = 510, ID_SAVE, ID_CANCEL,
    ID_KEY = 600, ID_PIN = 700, ID_OK = 800, ID_NO,
    ID_PREV = 900, ID_NEXT,
};

static int s_page;   /* shared by the paged lab screens */

static char s_note[48] = "";          /* last action, shown on the status row */
static char s_osk[40]  = "192.168.1.";
static bool s_shift    = false;

/* ---- fake terminal backdrop for the modal screens ------------------- */

static terminal_cell_t *s_term;

static void term_fill(int cols, int rows)
{
    static const char *LINES[] = {
        "deck@homelab:~$ journalctl -fu sshd",
        "Nov 12 14:02:11 homelab sshd[811]: Accepted publickey for deck",
        "Nov 12 14:02:11 homelab sshd[811]: pam_unix(sshd:session): opened",
        "Nov 12 14:03:40 homelab systemd[1]: Started Daily apt upgrade.",
        "Nov 12 14:04:02 homelab kernel: wlan0: associated",
        "deck@homelab:~$ htop",
        "  PID USER      PRI  NI  VIRT   RES   SHR S CPU% MEM%   TIME+",
        "  811 deck       20   0 16.2M 6304K 5120K S  0.7  0.3  0:00.42",
        "    1 root       20   0  167M 11.9M 8448K S  0.0  0.6  0:02.10",
    };
    for (int r = 0; r < rows; r++) {
        const char *l = LINES[r % NELEM(LINES)];
        const int n = (int)strlen(l);
        for (int c = 0; c < cols; c++)
            s_term[r * cols + c] = (terminal_cell_t){
                .cp = c < n ? (uint8_t)l[c] : ' ',
                .fg_color = LAB_FG, .bg_color = LAB_BG,
            };
    }
}

static void term_blank(int cols, int rows)
{
    for (int i = 0; i < cols * rows; i++)
        s_term[i] = (terminal_cell_t){ .cp = ' ', .fg_color = LAB_FG,
                                       .bg_color = LAB_BG };
}

/* Stand-in for the shell's StatusBar: chips left, clock right. */
static void lab_status(ui_rect_t r)
{
    const uint8_t bar = UI_BAR;
    ui_pen(OVERLAY_COL_BLUE);
    for (int x = r.x; x < r.x + r.w; x++)
        ui_putch(x, r.y, ' ', bar);

    int x = r.x + 1;
    ui_pen(OVERLAY_COL_GREEN);
    ui_puts(x, r.y, " NET ", UI_FOCUS | UI_BOLD);
    x += 5;
    static const uint16_t STAIR[4] = { 0x2582, 0x2584, 0x2586, 0x2588 };
    for (int i = 0; i < 4; i++)
        ui_putch(x++, r.y, i < 3 ? STAIR[i] : ' ', UI_FOCUS | UI_BOLD);
    ui_putch(x++, r.y, ' ', UI_FOCUS);
    ui_pen(OVERLAY_COL_BLUE);
    ui_putch(x++, r.y, ' ', bar);
    ui_pen(OVERLAY_COL_CYAN);
    ui_puts(x, r.y, " KBD ", UI_FOCUS | UI_BOLD);
    x += 5;
    ui_pen(OVERLAY_COL_AMBER);
    ui_putch(x++, r.y, 'C', UI_FOCUS | UI_BOLD);
    ui_pen(OVERLAY_COL_CYAN);
    ui_puts(x, r.y, "N ", UI_WELL | UI_BOLD);
    x += 3;

    if (s_note[0]) {
        ui_pen(OVERLAY_COL_BLUE);
        ui_text(ui_rect(x, r.y, r.x + r.w - 9 - x, 1), s_note, bar, UI_LEFT);
    }
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(r.x + r.w - 8, r.y, " 14:07 ", UI_FOCUS | UI_BOLD);
    ui_pen(OVERLAY_COL_DEFAULT);
}

const lab_profile_t PROFILES[LAB_NPROFILES] = {
    { "homelab",   "root@192.168.1.10:22" },
    { "build-box", "ci@build.lan:22  key" },
    { "pi-garage", "pi@10.0.0.42:22" },
    { "vps-fra",   "deploy@fra1.example.net:2222  key" },
    { "router",    "admin@192.168.1.1:22" },
    { "nas",       "backup@nas.lan:22  key" },
    { "printer",   "octo@10.0.0.77:22" },
    { "bastion",   "ops@bastion.example.org:22  key" },
};

/* HOME at the storage limit (8 profiles), three ways. @p row: the
 * bottom action row, else Settings is one more tile. @p two: two-line
 * cards, else one line with the address as the value. */
static void home_variant(bool row, bool two)
{
    const ui_metrics_t m = ui_metrics();
    ui_page_t p = ui_page();
    ui_rect_t body = p.body;
    ui_rect_t acts = row ? ui_cut_bottom(&body, m.unit) : ui_rect(0, 0, 0, 0);
    const ui_grid_t g = ui_grid(body, m.cls == UI_WIDE ? 3 : 2,
                                two ? 4 : m.unit, m.gutter, 0);
    const int count = NELEM(PROFILES) + (row ? 0 : 1);
    const int per   = ui_grid_capacity(&g);
    const int pages = ui_page_count(count, per);
    s_page = ui_page_clamp(s_page, count, per);

    ui_rect_t head = ui_pager(p.header, ID_PREV, ID_NEXT, s_page, pages);
    ui_header(head, NULL, "CYBERDECK", pages > 1 ? NULL : "v0.9", false);

    for (int slot = 0; slot < per; slot++) {
        const int i = s_page * per + slot;
        if (i >= count) break;
        if (i == NELEM(PROFILES)) {
            ui_card(ui_grid_cell(&g, slot), ID_SETTINGS, &(ui_card_t){
                .title = "Settings",
                .detail = two ? "wifi - profiles - keys" : NULL,
                .accent = OVERLAY_COL_CYAN, .flags = UI_CARD_NAV });
            continue;
        }
        ui_card(ui_grid_cell(&g, slot), (ui_id_t)(ID_PROFILE + i), &(ui_card_t){
            .title  = PROFILES[i].name,
            .detail = two ? PROFILES[i].where : NULL,
            .value  = two ? NULL : PROFILES[i].where,
            .accent = prof_accent(PROFILES[i].name) });
    }
    if (row) {
        const ui_grid_t a = ui_grid(acts, 3, m.unit, m.gutter, 0);
        ui_btn(ui_grid_cell(&a, 0), ID_NEW,      "+ New profile", OVERLAY_COL_GREEN);
        ui_btn(ui_grid_cell(&a, 1), ID_LOCK,     "Lock deck",     OVERLAY_COL_AMBER);
        ui_btn(ui_grid_cell(&a, 2), ID_SETTINGS, "Settings",      OVERLAY_COL_CYAN);
    }
    lab_status(p.status);
}

static void scr_home(void)       { home_variant(true,  true);  }
static void scr_home_tile(void)  { home_variant(false, true);  }
static void scr_home_1line(void) { home_variant(true,  false); }

static void scr_hub(void)
{
    static const struct { const char *t, *d; } HUB[] = {
        { "PROFILES", "add - edit - order" }, { "WIFI",      "HomeAP  -55 dBm" },
        { "KEYBOARD", "K380 connected" },     { "CRT FX",    "scanlines - phosphor" },
        { "MOTION FX", "glow - wipe" },       { "FONT",      "8x16  100x30" },
        { "SYSTEM",   "saver - reset" },      { "KEYSTORE",  "unlocked" },
    };
    const ui_metrics_t m = ui_metrics();
    ui_page_t p = ui_page();
    ui_header(p.header, NULL, "SETTINGS", NULL, true);
    ui_grid_t g = ui_grid(p.body, 2, 4, m.gutter, 0);
    for (int i = 0; i < NELEM(HUB); i++)
        ui_card(ui_grid_cell(&g, i), (ui_id_t)(ID_HUB + i), &(ui_card_t){
            .title = HUB[i].t, .detail = HUB[i].d,
            .accent = OVERLAY_COL_CYAN, .flags = UI_CARD_NAV });
    lab_status(p.status);
}

static void scr_section(void)
{
    static const struct { const char *t, *v; uint8_t f; } ITEMS[] = {
        { "Scanlines", "on", 0 },       { "Phosphor", "color", 0 },
        { "Bold pop", "on", 0 },        { "Glow build", "strong", 0 },
        { "Screen saver", "5 min", 0 }, { "Edge scroll", "off", UI_CARD_DISABLED },
    };
    const ui_metrics_t m = ui_metrics();
    ui_page_t p = ui_page();
    ui_header(p.header, "SETTINGS", "CRT FX", NULL, true);
    ui_grid_t g = ui_grid(p.body, 2, m.unit, m.gutter, 0);
    for (int i = 0; i < NELEM(ITEMS); i++)
        ui_card(ui_grid_cell(&g, i), (ui_id_t)(ID_VALUE + i), &(ui_card_t){
            .title = ITEMS[i].t, .value = ITEMS[i].v,
            .accent = OVERLAY_COL_CYAN, .flags = ITEMS[i].f });
    lab_status(p.status);
}

/* A profile picker at the storage limit: two columns hold all eight at
 * every grid, so it never pages. */
static void scr_list(void)
{
    const ui_metrics_t m = ui_metrics();
    ui_page_t p = ui_page();
    ui_header(p.header, "PROFILES", "EDIT", NULL, true);
    const ui_grid_t g = ui_grid(p.body, 2, 4, m.gutter, 0);
    for (int i = 0; i < NELEM(PROFILES) && i < ui_grid_capacity(&g); i++)
        ui_card(ui_grid_cell(&g, i), (ui_id_t)(ID_ROW + i), &(ui_card_t){
            .title = PROFILES[i].name, .detail = PROFILES[i].where,
            .accent = prof_accent(PROFILES[i].name), .flags = UI_CARD_NAV });
    lab_status(p.status);
}

static void scr_form(void)
{
    const ui_metrics_t m = ui_metrics();
    ui_page_t p = ui_page();
    ui_header(p.header, "PROFILES", "NEW PROFILE", NULL, true);
    ui_rect_t body = p.body;
    ui_rect_t acts = ui_cut_bottom(&body, m.unit);

    ui_grid_t g = ui_grid(body, 2, m.unit, m.gutter, 0);
    ui_input(ui_grid_cell(&g, 0), ID_FIELD + 0, "Name", "homelab", 0, 0);
    ui_input(ui_grid_cell(&g, 1), ID_FIELD + 1, "Host", "192.168.1.", 10,
             UI_INPUT_EDITING);
    ui_input(ui_grid_cell(&g, 2), ID_FIELD + 2, "Port", "22", 0, 0);
    ui_input(ui_grid_cell(&g, 3), ID_FIELD + 3, "User", "root", 0, 0);
    ui_input(ui_grid_cell(&g, 4), ID_AUTH, "Auth", "password", 0,
             UI_INPUT_CYCLE);
    ui_input(ui_grid_cell(&g, 5), ID_FIELD + 4, "Pass", "hunter2", 0,
             UI_INPUT_MASK);

    ui_grid_t a = ui_grid(ui_center(acts, acts.w * 2 / 3, m.unit), 2, m.unit,
                          m.gutter, 0);
    ui_btn(ui_grid_cell(&a, 0), ID_SAVE,   "Save",   OVERLAY_COL_GREEN);
    ui_btn(ui_grid_cell(&a, 1), ID_CANCEL, "Cancel", OVERLAY_COL_BLUE);
    lab_status(p.status);
}

/* On-screen keyboard rows; \b backspace, \t shift, \n done, \1 symbols. */
static const char *const OSK_ROWS[4] = {
    "qwertyuiop", "asdfghjkl\b", "\tzxcvbnm.-", "\1 @\n",
};

static void scr_osk(void)
{
    const ui_metrics_t m = ui_metrics();
    ui_page_t p = ui_page();
    ui_header(p.header, "NEW PROFILE", "HOST", NULL, true);
    ui_rect_t body = p.body;
    ui_input(ui_cut_top(&body, m.unit), ID_FIELD, "Host", s_osk,
             (int)strlen(s_osk), UI_INPUT_EDITING);

    const int kw = body.w / 10;
    body = ui_center(body, kw * 10, 4 * m.unit);
    for (int r = 0; r < 4; r++) {
        ui_rect_t row = ui_cut_top(&body, m.unit);
        for (const char *k = OSK_ROWS[r]; *k; k++) {
            char label[8] = { *k, 0 };
            uint8_t flags = UI_CARD_CENTER, accent = OVERLAY_COL_DEFAULT;
            int w = kw;
            if (*k >= 'a' && *k <= 'z' && s_shift) label[0] = (char)(*k - 32);
            if (*k == '\b') { strcpy(label, "<-"); accent = OVERLAY_COL_AMBER; }
            if (*k == '\t') { strcpy(label, "Aa"); accent = OVERLAY_COL_BLUE; }
            if (*k == '\1') { strcpy(label, "123"); accent = OVERLAY_COL_BLUE; w = 2 * kw; }
            if (*k == ' ')  { strcpy(label, "space"); w = 5 * kw; }
            if (*k == '@')  { w = kw; }
            if (*k == '\n') { strcpy(label, "Done"); accent = OVERLAY_COL_GREEN;
                              flags |= UI_CARD_ACTION; w = 2 * kw; }
            ui_rect_t key = ui_cut_left(&row, w);
            key.w -= 1;                          /* 1-cell gap between keys */
            ui_card(key, (ui_id_t)(ID_KEY + (uint8_t)*k),
                    &(ui_card_t){ .title = label, .accent = accent,
                                  .flags = flags });
        }
    }
    lab_status(p.status);
}

static void scr_pin(void)
{
    static const char *PAD[12] = { "1", "2", "3", "4", "5", "6",
                                   "7", "8", "9", "<-", "0", "OK" };
    const ui_metrics_t m = ui_metrics();
    ui_page_t p = ui_page();
    ui_header(p.header, NULL, "UNLOCK DECK", "keystore", false);
    ui_rect_t body = p.body;

    /* Entry row: filled and empty slots, centered on a real row. */
    ui_rect_t entry = ui_cut_top(&body, m.unit);
    const int slots = 6, filled = 3;
    int x = entry.x + (entry.w - (slots * 4 - 2)) / 2;
    for (int i = 0; i < slots; i++, x += 4) {
        ui_pen(i < filled ? OVERLAY_COL_CYAN : OVERLAY_COL_BLUE);
        ui_putch(x, entry.y + 1, i < filled ? UI_LED_ON : UI_LED_OFF,
                 i < filled ? UI_TEXT : UI_MUTED);
        ui_putch(x + 1, entry.y + 1, ' ', UI_TEXT);
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    const int kw = m.cls == UI_WIDE ? 14 : 12;
    ui_grid_t g = ui_grid(ui_center(body, 3 * kw + 2 * m.gutter, 4 * m.unit),
                          3, m.unit, m.gutter, 0);
    for (int i = 0; i < 12; i++) {
        uint8_t accent = OVERLAY_COL_DEFAULT, flags = UI_CARD_CENTER;
        if (i == 9)  accent = OVERLAY_COL_AMBER;
        if (i == 11) { accent = OVERLAY_COL_GREEN; flags |= UI_CARD_ACTION; }
        ui_card(ui_grid_cell(&g, i), (ui_id_t)(ID_PIN + i), &(ui_card_t){
            .title = PAD[i], .accent = accent, .flags = flags });
    }
    lab_status(p.status);
}

static void scr_dialog(void)
{
    const ui_metrics_t m = ui_metrics();
    const int w = m.cls == UI_COMPACT ? 50 : 56;
    ui_rect_t in = ui_dialog(w, 11, "Disconnect session?", OVERLAY_COL_AMBER);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_text(ui_rect(in.x, in.y, in.w, 1), "homelab is still running htop.",
            UI_TEXT, UI_LEFT);
    ui_text(ui_rect(in.x, in.y + 1, in.w, 1), "The remote shell will be closed.",
            UI_MUTED, UI_LEFT);
    ui_rect_t acts = ui_cut_bottom(&in, m.unit);
    ui_grid_t a = ui_grid(acts, 2, m.unit, m.gutter, 0);
    ui_btn(ui_grid_cell(&a, 0), ID_OK, "Disconnect", OVERLAY_COL_RED);
    ui_btn(ui_grid_cell(&a, 1), ID_NO, "Stay",       OVERLAY_COL_BLUE);
}

static void scr_connect(void)
{
    static const struct { const char *t; int done; } STEPS[] = {
        { "wifi link", 1 }, { "tcp 192.168.1.10:22", 1 },
        { "host key  SHA256:9fK2..Qm1c", 1 }, { "authenticating", 0 },
    };
    const ui_metrics_t m = ui_metrics();
    ui_page_t p = ui_page();
    ui_header(p.header, NULL, "CONNECTING", "homelab", true);
    ui_rect_t body = ui_center(p.body, p.body.w * 3 / 4, p.body.h);
    ui_rect_t acts = ui_cut_bottom(&body, m.unit);
    ui_cut_top(&body, 1);
    for (int i = 0; i < NELEM(STEPS); i++) {
        ui_rect_t row = ui_cut_top(&body, 2);
        ui_pen(STEPS[i].done ? OVERLAY_COL_GREEN : OVERLAY_COL_AMBER);
        ui_putch(row.x, row.y, STEPS[i].done ? UI_CHECK : spinner_glyph(3),
                 UI_TEXT | UI_BOLD);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_text(ui_rect(row.x + 3, row.y, row.w - 3, 1), STEPS[i].t,
                STEPS[i].done ? UI_MUTED : UI_TEXT | UI_BOLD, UI_LEFT);
    }
    ui_meter(ui_cut_top(&body, 1), 7, 10, OVERLAY_COL_GREEN);
    ui_btn(ui_center(acts, acts.w / 2, m.unit), ID_CANCEL, "Cancel",
           OVERLAY_COL_BLUE);
    lab_status(p.status);
}

/* Every card state side by side: the sheet that decides a rendition. */
static void scr_kit(void)
{
    const ui_metrics_t m = ui_metrics();
    ui_page_t p = ui_page();
    ui_header(p.header, "KIT", "CARD STATES", "focus / press / off", true);
    ui_grid_t g = ui_grid(p.body, m.cls == UI_WIDE ? 4 : 3, 4, m.gutter, 0);
    static const struct { const char *t, *d; uint8_t a, f; } C[] = {
        { "Normal",   "title + detail",  OVERLAY_COL_CYAN,    0 },
        { "Focused",  "keyboard ring",   OVERLAY_COL_CYAN,    0 },
        { "Pressed",  "finger down",     OVERLAY_COL_CYAN,    0 },
        { "Disabled", "no input",        OVERLAY_COL_CYAN,    UI_CARD_DISABLED },
        { "Checked",  "current choice",  OVERLAY_COL_GREEN,   UI_CARD_CHECKED },
        { "Opens",    "another page",    OVERLAY_COL_MAGENTA, UI_CARD_NAV },
        { "Amber",    "identity accent", OVERLAY_COL_AMBER,   0 },
        { "Blue",     "identity accent", OVERLAY_COL_BLUE,    0 },
    };
    int n = 0;
    for (; n < NELEM(C) && n < ui_grid_capacity(&g) - 3; n++)
        ui_card(ui_grid_cell(&g, n), (ui_id_t)(ID_HUB + n), &(ui_card_t){
            .title = C[n].t, .detail = C[n].d, .accent = C[n].a,
            .flags = C[n].f });
    ui_rect_t b = ui_grid_cell(&g, n++);
    ui_btn(ui_rect(b.x, b.y, b.w, m.unit), ID_OK, "Confirm", OVERLAY_COL_GREEN);
    b = ui_grid_cell(&g, n++);
    ui_btn(ui_rect(b.x, b.y, b.w, m.unit), ID_NO, "Delete",  OVERLAY_COL_RED);
    ui_card(ui_grid_cell(&g, n++), ID_VALUE, &(ui_card_t){
        .title = "Scanlines", .value = "on", .accent = OVERLAY_COL_CYAN });
    lab_status(p.status);
}

/* Block and box glyphs the half-cell look depends on, at this font. */
static void scr_glyphs(void)
{
    ui_page_t p = ui_page();
    ui_header(p.header, "KIT", "GLYPHS", "U+2580..259F / box / marks", true);
    static const struct { uint16_t from, to; } RANGES[] = {
        { 0x2580, 0x259F }, { 0x2500, 0x251F }, { 0x2550, 0x256F },
        { 0x25A0, 0x25CF }, { 0x2190, 0x2195 }, { 0x2713, 0x2718 },
        { 0x2039, 0x203A }, { 0x2026, 0x2026 }, { 0xE0B0, 0xE0B3 },
    };
    int x = p.body.x, y = p.body.y;
    for (int i = 0; i < NELEM(RANGES); i++) {
        for (unsigned cp = RANGES[i].from; cp <= RANGES[i].to; cp++) {
            ui_pen(OVERLAY_COL_CYAN);
            ui_putch(x, y, (uint16_t)cp, UI_TEXT);
            ui_putch(x + 1, y, (uint16_t)cp, UI_BAR);
            x += 3;
            if (x + 3 > p.body.x + p.body.w) { x = p.body.x; y += 2; }
        }
        x = p.body.x;
        y += 2;
        if (y >= p.body.y + p.body.h) break;
    }
    ui_pen(OVERLAY_COL_DEFAULT);
    lab_status(p.status);
}

static const lab_screen_t SCREENS[] = {
    { "home",    scr_home },
    { "home-tile",  scr_home_tile },
    { "home-1line", scr_home_1line },
    { "hub",     scr_hub,     .focus = ID_HUB + 3 },
    { "section", scr_section, .down = ID_VALUE + 2 },
    { "list",    scr_list },
    { "form",    scr_form },
    { "osk",     scr_osk,     .down = ID_KEY + 'g' },
    { "pin",     scr_pin },
    { "dialog",  scr_dialog,  .term = true },
    { "connect", scr_connect },
    { "kit",     scr_kit,     .focus = ID_HUB + 1, .down = ID_HUB + 2 },
    { "glyphs",  scr_glyphs },
};

static int s_cols, s_rows;
uint32_t lab_frame;
static bool s_faulted;        /* a screen counted a layout fault     */

static int screen_count(void)
{
    int n;
    lab_mo_screens(&n);
    return NELEM(SCREENS) + n;
}

static const lab_screen_t *screen_at(int i)
{
    int n;
    const lab_screen_t *mo = lab_mo_screens(&n);
    return i < NELEM(SCREENS) ? &SCREENS[i] : &mo[i - NELEM(SCREENS)];
}

/* Every render bakes its own theme: the dump and other screens leave
 * another one active. */
static void render(const lab_screen_t *s, uint32_t frame)
{
    if (s->slate) ui_colors(LAB_SLATE_FG, LAB_SLATE_BG);
    else          ui_colors(LAB_FG, LAB_BG);
    if (s->term) term_fill(s_cols, s_rows);
    else         term_blank(s_cols, s_rows);
    lab_frame = frame;
    ui_frame(frame);
    ui_clear();
    s->draw();
    ui_present();
}

/* Cell at the center of region @p id in the frame just drawn. */
static bool region_center(ui_id_t id, int *col, int *row)
{
    for (int r = 0; r < s_rows; r++)
        for (int c = 0; c < s_cols; c++)
            if (ui_hot_at(c, r) == id) {
                int c1 = c, r1 = r;
                while (c1 + 1 < s_cols && ui_hot_at(c1 + 1, r) == id) c1++;
                while (r1 + 1 < s_rows && ui_hot_at(c, r1 + 1) == id) r1++;
                *col = (c + c1) / 2;
                *row = (r + r1) / 2;
                return true;
            }
    return false;
}

/* Draw a screen in its documented pose. The first pass lays out the
 * regions. The second draws with the focus and the held finger set. */
static void render_posed(const lab_screen_t *s)
{
    const int faults = ui_faults();
    ui_hot_reset();
    render(s, 0);
    ui_hot_touch(UI_TOUCH_UP, 0, 0);            /* touch mode: no ring */
    int c, r;
    if (s->down && region_center(s->down, &c, &r))
        ui_hot_touch(UI_TOUCH_DOWN, c, r);
    if (s->focus) {
        ui_focus_set(s->focus);
        ui_hot_key(K_ENTER);                     /* only reveals the ring */
    }
    render(s, 1);
    if (ui_faults() > faults) {
        fprintf(stderr, "%s: layout faults\n", s->name);
        s_faulted = true;
    }
}

/* Apply a press in the interactive lab: enough behavior to feel it. */
static void lab_press(ui_id_t id)
{
    if (id == ID_PREV) s_page--;
    if (id == ID_NEXT) s_page++;
    if (id >= ID_KEY && id < ID_KEY + 128) {
        const char k = (char)(id - ID_KEY);
        size_t n = strlen(s_osk);
        if (k == '\b')      { if (n) s_osk[n - 1] = 0; }
        else if (k == '\t') s_shift = !s_shift;
        else if (k == '\n' || k == '\1') { /* no-op in the lab */ }
        else if (n + 1 < sizeof s_osk) {
            s_osk[n] = (k >= 'a' && k <= 'z' && s_shift) ? (char)(k - 32) : k;
            s_osk[n + 1] = 0;
            s_shift = false;
        }
    }
    snprintf(s_note, sizeof s_note, id == UI_ID_BACK ? " back" : " press %u",
             (unsigned)id);
}

/* ---- model dump for tools/uimock ------------------------------------- */

static const char *const STYLE_NAMES[] = {
    "TEXT", "MUTED", "BAR", "WELL", "FOCUS", "TRACK", "BAR_EDGE",
    "WELL_EDGE", "FOCUS_EDGE", "PANEL", "PANEL_MUTED", "PANEL_EDGE",
};
_Static_assert(NELEM(STYLE_NAMES) == UI_STYLE_COUNT, "name every style");

/* Every glyph the active font can draw, as decoded rows. A codepoint
 * that decodes to the '?' fallback is absent and left out. */
static void dump_font(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/font-%dx%d.json", dir, font_width(),
             font_height());
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }

    const int h = font_height(), rb = (int)FONT_ROW_BYTES(font_width());
    const size_t nb = font_glyph_bytes();
    _Alignas(4) uint8_t q[FONT_MAX_GLYPH_BYTES], g[FONT_MAX_GLYPH_BYTES],
                        b[FONT_MAX_GLYPH_BYTES];
    font_decode_glyph('?', false, q);

    fprintf(f, "{\"w\":%d,\"h\":%d,\"sprite_base\":%u,\"sprite_count\":%u",
            font_width(), h, FONT_SPRITE_BASE, FONT_SPRITE_COUNT);
    for (int bold = 0; bold < 2; bold++) {
        fprintf(f, ",\n\"%s\":{", bold ? "bold" : "regular");
        bool first = true;
        for (unsigned cp = 0x20; cp <= 0xFFFF; cp++) {
            if (cp - FONT_SPRITE_BASE < FONT_SPRITE_COUNT) continue;
            font_decode_glyph((uint16_t)cp, false, g);
            if (cp != '?' && !memcmp(g, q, nb)) continue;
            const uint8_t *src = g;
            if (bold) {
                font_decode_glyph((uint16_t)cp, true, b);
                if (!memcmp(g, b, nb)) continue;   /* no bold form */
                src = b;
            }
            fprintf(f, "%s\"%u\":\"", first ? "" : ",", cp);
            for (int r = 0; r < h; r++) {
                if (rb == 1) fprintf(f, "%02x", src[r]);
                else fprintf(f, "%04x", src[r * 2] | (src[r * 2 + 1] << 8));
            }
            fputc('"', f);
            first = false;
        }
        fputc('}', f);
    }
    fprintf(f, "}\n");
    fclose(f);
}

static void dump_theme(FILE *f, const char *name, color_t fg, color_t bg,
                       bool last)
{
    ui_colors(fg, bg);
    const display_overlay_style_t *st = ui_debug_styles();
    fprintf(f, "\"%s\":{\"fg\":%u,\"bg\":%u,\"table\":[", name, fg, bg);
    for (int i = 0; i < UI_PAL_COUNT; i++)
        fprintf(f, "%s%u,%u", i ? "," : "", st[i].fg, st[i].bg);
    fprintf(f, "]}%s\n", last ? "" : ",");
}

static FILE *dump_open(const char *dir)
{
    char path[512];
    snprintf(path, sizeof path, "%s/lab-%dx%d.json", dir, font_width(),
             font_height());
    FILE *f = fopen(path, "w");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(1); }

    const ui_metrics_t m = ui_metrics();
    fprintf(f, "{\"cw\":%d,\"ch\":%d,\"cols\":%d,\"rows\":%d,"
            "\"width\":%d,\"height\":%d,\n",
            font_width(), font_height(), s_cols, s_rows,
            DISPLAY_WIDTH, DISPLAY_HEIGHT);
    fprintf(f, "\"metrics\":{\"cls\":%d,\"margin\":%d,\"gutter\":%d,"
            "\"unit\":%d,\"header\":%d},\n",
            m.cls, m.margin, m.gutter, m.unit, m.header);
    fprintf(f, "\"accents\":%d,\"styles\":[", OVERLAY_ACCENTS);
    for (int i = 0; i < UI_STYLE_COUNT; i++)
        fprintf(f, "%s\"%s\"", i ? "," : "", STYLE_NAMES[i]);
    fprintf(f, "],\n\"themes\":{\n");
    dump_theme(f, "hazard", COLOR_WHITE, RGB565(96, 0, 0), false);
    dump_theme(f, "default", LAB_FG, LAB_BG, false);
    dump_theme(f, "slate", LAB_SLATE_FG, LAB_SLATE_BG, true);
    fprintf(f, "},\n\"screens\":[\n");
    return f;
}

/* One posed frame: overlay cells, the terminal under it, hit regions. */
static void dump_screen(FILE *f, const lab_screen_t *s, bool first)
{
    bool scrim = false;
    const display_overlay_cell_t *ov = ui_debug_frame(&scrim);
    const int n = s_cols * s_rows;

    fprintf(f, "%s{\"name\":\"%s\",\"theme\":\"%s\",\"scrim\":%s,"
            "\"focus\":%u,\"down\":%u,\n\"ov\":[", first ? "" : ",\n",
            s->name, s->slate ? "slate" : "default",
            scrim ? "true" : "false", s->focus, s->down);
    for (int i = 0; i < n; i++)
        fprintf(f, "%s%u,%u,%u", i ? "," : "", ov[i].cp, ov[i].attrs,
                ov[i].pal);
    fprintf(f, "],\n\"term\":[");
    for (int i = 0; i < n; i++)
        fprintf(f, "%s%u,%u,%u,%u", i ? "," : "", s_term[i].cp,
                s_term[i].fg_color, s_term[i].bg_color, s_term[i].attrs);

    /* Regions as the finger meets them: the bounding box of each id. */
    struct { ui_id_t id; int x0, y0, x1, y1; } box[UI_HOT_MAX];
    int nbox = 0;
    for (int r = 0; r < s_rows; r++) {
        for (int c = 0; c < s_cols; c++) {
            const ui_id_t id = ui_hot_at(c, r);
            if (!id) continue;
            int k = 0;
            while (k < nbox && box[k].id != id) k++;
            if (k == nbox) {
                if (nbox == UI_HOT_MAX) continue;
                box[nbox].id = id;
                box[nbox].x0 = box[nbox].x1 = c;
                box[nbox].y0 = box[nbox].y1 = r;
                nbox++;
                continue;
            }
            if (c < box[k].x0) box[k].x0 = c;
            if (c > box[k].x1) box[k].x1 = c;
            if (r > box[k].y1) box[k].y1 = r;
        }
    }
    fprintf(f, "],\n\"hot\":[");
    for (int k = 0; k < nbox; k++)
        fprintf(f, "%s[%u,%d,%d,%d,%d]", k ? "," : "", box[k].id,
                box[k].x0, box[k].y0, box[k].x1 - box[k].x0 + 1,
                box[k].y1 - box[k].y0 + 1);
    fprintf(f, "]}");
}

static int find_screen(const char *name)
{
    for (int i = 0; i < screen_count(); i++)
        if (!strcmp(screen_at(i)->name, name)) return i;
    return -1;
}

int main(int argc, char *argv[])
{
    font_size_t font = FONT_SIZE_8X16;
    const char *out = NULL, *only = NULL, *dump = NULL;
    for (int i = 1; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : "";
        if (!strcmp(argv[i], "--font")) {
            font = !strcmp(v, "12x24") ? FONT_SIZE_12X24
                 : !strcmp(v, "10x20") ? FONT_SIZE_10X20 : FONT_SIZE_8X16;
            i++;
        } else if (!strcmp(argv[i], "--out"))    { out = v;  i++; }
        else if (!strcmp(argv[i], "--dump"))     { dump = v; i++; }
        else if (!strcmp(argv[i], "--screen"))   { only = v; i++; }
        else {
            fprintf(stderr, "usage: cyberdeck_uilab [--font WxH] "
                    "[--out DIR] [--dump DIR] [--screen NAME]\n");
            return 2;
        }
    }

    font_init(font);
    display_render_set_font(font_width(), font_height());
    s_cols = display_text_cols();
    s_rows = display_text_rows();
    s_term = calloc((size_t)s_cols * s_rows, sizeof *s_term);
    if (!s_term) return 1;
    term_blank(s_cols, s_rows);
    display_set_text_buffer(s_term, s_cols, s_rows);
    display_set_cursor(-1, -1, CURSOR_NONE);

    /* A shot must not depend on its capture time: keep the static CRT
     * look, drop every animated effect. */
    display_fx_cfg_t fx;
    display_fx_defaults(&fx);
    fx.glow = fx.wipe = fx.collapse = fx.static_burst = fx.wobble = 0;
    display_fx_set(&fx);

    if (ui_init() != ESP_OK) return 1;
    ui_colors(LAB_FG, LAB_BG);

    int cur = only ? find_screen(only) : 0;
    if (cur < 0) { fprintf(stderr, "no screen '%s'\n", only); return 2; }

    if (out || dump) {
        FILE *df = NULL;
        if (dump) {
            dump_font(dump);
            df = dump_open(dump);
        }
        bool first = true;
        for (int i = 0; i < screen_count(); i++) {
            if (only && i != cur) continue;
            char path[512];
            render_posed(screen_at(i));
            if (out) {
                snprintf(path, sizeof path, "%s/%s.bmp", out,
                         screen_at(i)->name);
                if (display_screenshot_bmp(path) != ESP_OK) {
                    fprintf(stderr, "cannot write %s\n", path);
                    return 1;
                }
            }
            if (df) dump_screen(df, screen_at(i), first);
            first = false;
        }
        if (df) { fprintf(df, "\n]}\n"); fclose(df); }
        return s_faulted ? 1 : 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0 || display_init() != ESP_OK) return 1;
    ui_hot_reset();
    bool run = true, dirty = true;
    uint32_t last_frame = 0;
    while (run) {
        const uint32_t frame = (uint32_t)(SDL_GetTicks64() / 100);
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            uint16_t fx_, fy_;
            ui_act_t a = { 0 };
            switch (ev.type) {
            case SDL_QUIT: run = false; break;
            case SDL_MOUSEBUTTONDOWN:
                display_window_to_fb(ev.button.x, ev.button.y, &fx_, &fy_);
                a = ui_touch_px(ev.button.button == SDL_BUTTON_RIGHT
                                ? UI_TOUCH_HOLD : UI_TOUCH_DOWN, fx_, fy_);
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button != SDL_BUTTON_LEFT) break;
                display_window_to_fb(ev.button.x, ev.button.y, &fx_, &fy_);
                a = ui_touch_px(UI_TOUCH_TAP, fx_, fy_);
                break;
            case SDL_KEYDOWN:
                switch (ev.key.keysym.sym) {
                case SDLK_ESCAPE: run = false; break;
                case SDLK_TAB:
                    cur = (cur + ((ev.key.keysym.mod & KMOD_SHIFT)
                                  ? screen_count() - 1 : 1)) % screen_count();
                    ui_hot_reset();
                    s_note[0] = 0;
                    break;
                case SDLK_UP:     a = ui_hot_key(K_UP);    break;
                case SDLK_DOWN:   a = ui_hot_key(K_DOWN);  break;
                case SDLK_LEFT:   a = ui_hot_key(K_LEFT);  break;
                case SDLK_RIGHT:  a = ui_hot_key(K_RIGHT); break;
                case SDLK_RETURN: a = ui_hot_key(K_ENTER); break;
                default: break;
                }
                break;
            default: break;
            }
            if (a.kind == UI_ACT_PRESS) lab_press(a.id);
            dirty = true;
        }
        if (dirty || frame != last_frame || ui_hot_animating()) {
            render(screen_at(cur), frame);
            last_frame = frame;
            dirty = false;
        }
        display_render_frame();
    }
    SDL_Quit();
    return 0;
}
