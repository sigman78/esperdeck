/*
 * app_profile.c — on-device profile editor (ST_PROFILE): text fields,
 * auth-mode toggle, stored-key picker, validation + persist.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "app_menu_defs.h"   /* the editor exits back into MS_PROFILES */
#include "font.h"            /* form-row touch mapping */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"   /* key-id list lives in SPIRAM */

/* Field order: text fields, then the two selector rows (auth toggle + key
 * picker), then Save/Cancel — one up/down focus ring. The key row only
 * exists while auth == key. */
typedef enum {
    PF_NAME = 0, PF_HOST, PF_PORT, PF_USER, PF_AUTH, PF_PASS, PF_KEY,
    PF_SAVE, PF_CANCEL, PF_COUNT,
} pf_field_t;
#define PF_ROWS  PF_SAVE          /* form rows drawn above the buttons */

#define PF_KEY_MAX 8              /* key ids offered by the picker */

static bool pf_is_text(int f)
{
    return f <= PF_USER || f == PF_PASS;
}

/* Is @p f part of the focus ring under the current auth mode? */
static bool pf_field_present(int f)
{
    return f != PF_KEY || app.pf.draft.auth == STORAGE_AUTH_KEY;
}

/* Resolve a text field's label, buffer, max length and flags. */
static char *pf_buf(int i, const char **label, int *max,
                    bool *numeric, bool *mask)
{
    *numeric = false; *mask = false;
    switch (i) {
    case PF_NAME: *label = "Name"; *max = sizeof(app.pf.draft.name) - 1;
                  return app.pf.draft.name;
    case PF_HOST: *label = "Host"; *max = sizeof(app.pf.draft.host) - 1;
                  return app.pf.draft.host;
    case PF_PORT: *label = "Port"; *max = 5; *numeric = true;
                  return app.pf.port;
    case PF_USER: *label = "User"; *max = sizeof(app.pf.draft.user) - 1;
                  return app.pf.draft.user;
    case PF_PASS: /* doubles as the key passphrase under key auth */
                  *label = app.pf.draft.auth == STORAGE_AUTH_KEY
                           ? "Phrase" : "Pass";
                  *max = sizeof(app.pf.draft.password) - 1;
                  *mask = true; return app.pf.draft.password;
    }
    *label = ""; *max = 0; return NULL;
}

/* (Re)load the key picker's id list and cached type of the selection. */
static void pf_key_refresh_info(void)
{
    app.pf.key_type[0] = '\0';
    if (app.pf.key_sel >= 0 && app.pf.key_sel < app.pf.nkeys)
        storage_key_info(app.pf.keys[app.pf.key_sel],
                         app.pf.key_type, sizeof(app.pf.key_type), NULL, 0);
}

static void pf_load_keys(void)
{
    if (!app.pf.keys)
        app.pf.keys = heap_caps_malloc(PF_KEY_MAX * STORAGE_KEY_ID_LEN,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    app.pf.nkeys = 0;
    if (app.pf.keys)
        storage_list_keys(app.pf.keys, PF_KEY_MAX, &app.pf.nkeys);

    /* Preselect the draft's key when editing; a draft without one starts on
     * the first stored key (adopted only when auth is toggled to key). */
    app.pf.key_sel = -1;
    for (int i = 0; i < app.pf.nkeys; i++)
        if (strcmp(app.pf.keys[i], app.pf.draft.key_id) == 0) {
            app.pf.key_sel = i;
            break;
        }
    if (app.pf.key_sel < 0 && !app.pf.draft.key_id[0] && app.pf.nkeys > 0)
        app.pf.key_sel = 0;
    pf_key_refresh_info();
}

static void pf_auth_toggle(void)
{
    if (app.pf.draft.auth == STORAGE_AUTH_KEY) {
        app.pf.draft.auth = STORAGE_AUTH_PASSWORD;
    } else {
        app.pf.draft.auth = STORAGE_AUTH_KEY;
        if (app.pf.key_sel >= 0 && app.pf.key_sel < app.pf.nkeys)
            snprintf(app.pf.draft.key_id, sizeof(app.pf.draft.key_id), "%s",
                     app.pf.keys[app.pf.key_sel]);
    }
}

static void pf_key_cycle(int dir)
{
    if (app.pf.nkeys <= 0) return;
    app.pf.key_sel = (app.pf.key_sel < 0)
              ? (dir > 0 ? 0 : app.pf.nkeys - 1)
              : (app.pf.key_sel + dir + app.pf.nkeys) % app.pf.nkeys;
    snprintf(app.pf.draft.key_id, sizeof(app.pf.draft.key_id), "%s", app.pf.keys[app.pf.key_sel]);
    pf_key_refresh_info();
}

/* Form geometry — derived from the grid so the editor fits 100x30 down to
 * 66x20: wide grids center the form, narrow ones hug the left edge and drop
 * to single-row field spacing. The touch hit-test shares these. */
static int pf_x0(void)   { return ui_cols() >= 97 ? 26 : 2; }   /* form left  */
static int pf_fx(void)   { return pf_x0() + 8; }                /* field left */
static int pf_fw(void)   { int w = ui_cols() - pf_fx() - 2;     /* field width */
                           return w > 40 ? 40 : w; }
static int pf_y0(void)   { return ui_rows() >= 28 ? 6 : 5; }    /* first row  */
static int pf_step(void) { return ui_rows() >= 22 ? 2 : 1; }    /* row pitch  */

/* A selector row: '<' value '>' as a solid bar, inverted when focused. */
static void pf_draw_selector(int row, bool focused, const char *value)
{
    int fw = pf_fw();
    char bar[64];
    snprintf(bar, sizeof(bar), "< %-*.*s >", fw - 4, fw - 4, value);
    ui_puts(pf_fx(), row, bar, focused ? OVERLAY_ATTR_INVERSE : 0);
}

static void render_profile(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_screen_header(app.pf.edit_idx >= 0 ? "EDIT PROFILE" : "NEW PROFILE",
                       "// SSH DECK");

    for (int i = 0; i < PF_ROWS; i++) {
        int row = pf_y0() + i * pf_step();
        bool focused = (app.pf.field == i);
        if (pf_is_text(i)) {
            const char *label; int max; bool numeric, mask;
            char *buf = pf_buf(i, &label, &max, &numeric, &mask);
            ui_pen(focused ? OVERLAY_COL_CYAN : OVERLAY_COL_DEFAULT);
            ui_puts(pf_x0(), row, label, 0);
            /* Only the focused field's caret drives scrolling. */
            ui_field(pf_fx(), row, pf_fw(), buf, focused ? app.pf.cursor : 0,
                     focused, mask);
        } else if (i == PF_AUTH) {
            ui_pen(focused ? OVERLAY_COL_CYAN : OVERLAY_COL_DEFAULT);
            ui_puts(pf_x0(), row, "Auth", 0);
            pf_draw_selector(row, focused,
                             app.pf.draft.auth == STORAGE_AUTH_KEY
                             ? "key" : "password");
        } else if (i == PF_KEY && app.pf.draft.auth == STORAGE_AUTH_KEY) {
            ui_pen(focused ? OVERLAY_COL_CYAN : OVERLAY_COL_DEFAULT);
            ui_puts(pf_x0(), row, "Key", 0);
            char v[64];
            if (app.pf.key_sel >= 0 && app.pf.key_sel < app.pf.nkeys)
                snprintf(v, sizeof(v), "%s%s%s", app.pf.keys[app.pf.key_sel],
                         app.pf.key_type[0] ? "  " : "", app.pf.key_type);
            else if (app.pf.draft.key_id[0])
                snprintf(v, sizeof(v), "%s (missing)", app.pf.draft.key_id);
            else
                snprintf(v, sizeof(v), "(no keys - use Import)");
            pf_draw_selector(row, focused, v);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    /* Inline validation error — a modal has no toast strip, so a failed
     * Save must report here or it looks dead. */
    if (app.pf.err[0]) {
        ui_pen(OVERLAY_COL_RED);
        ui_puts(pf_x0(), pf_y0() + PF_ROWS * pf_step(), app.pf.err, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* Save / Cancel buttons — a 2-wide tile grid stashed for touch. */
    tilegrid_t bg = { .y0 = pf_y0() + PF_ROWS * pf_step() + 1, .tw = 20,
                      .th = ui_rows() >= 28 ? 3 : 2,
                      .gx = 4, .gy = 0, .ncols = 2, .nrows = 1, .count = 2 };
    bg.x0 = (ui_cols() - (bg.tw * 2 + bg.gx)) / 2;
    app.grid = bg;
    ui_pen(OVERLAY_COL_GREEN);
    ui_tile(tile_x(&bg, 0), tile_y(&bg, 0), bg.tw, bg.th, "Save", "",
            app.pf.field == PF_SAVE);
    ui_pen(OVERLAY_COL_BLUE);   /* safe navigation — matches menu Back */
    ui_tile(tile_x(&bg, 1), tile_y(&bg, 1), bg.tw, bg.th, "Cancel", "",
            app.pf.field == PF_CANCEL);
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_footer("type to edit \xB7 Tab/arrows move \xB7 Enter next \xB7 Esc cancel");
    ui_no_cursor();
    ui_present();
}

/* Open the editor: @p edit_idx = stored profile to edit, or -1 for a new
 * one. Remembers whether it was entered from the menu (to return there). */
void enter_profile(uint64_t now, int edit_idx)
{
    memset(&app.pf.draft, 0, sizeof(app.pf.draft));
    app.pf.return_menu  = (app.state == ST_MENU);
    app.pf.edit_idx     = (edit_idx >= 0 && edit_idx < app.stored_count)
                   ? edit_idx : -1;
    app.pf.orig_name[0] = '\0';
    if (app.pf.edit_idx >= 0) {
        app.pf.draft = app.profiles[app.pf.edit_idx];
        snprintf(app.pf.orig_name, sizeof(app.pf.orig_name), "%s", app.pf.draft.name);
        snprintf(app.pf.port, sizeof(app.pf.port), "%u", (unsigned)app.pf.draft.port);
    } else {
        snprintf(app.pf.port, sizeof(app.pf.port), "22");
    }
    pf_load_keys();
    app.pf.field  = PF_NAME;
    app.pf.cursor = (int)strlen(app.pf.draft.name);
    app.pf.err[0] = '\0';
    app.state = ST_PROFILE;
    (void)now;
    render_profile();
}

/* Validate the draft and persist it: append for a new profile, replace the
 * original (found by its entry-time name) for an edit. Returns "" on success
 * or a short reason to show inline on failure. */
static const char *profile_commit(void)
{
    if (app.pf.draft.name[0] == '\0') return "name required";
    /* A '[' or ']' in the name breaks the INI section header on save and
     * silently corrupts the file on reload. Reject them. */
    if (strpbrk(app.pf.draft.name, "[]")) return "name: no [ or ]";
    if (app.pf.draft.host[0] == '\0') return "host required";
    if (app.pf.draft.user[0] == '\0') return "user required";
    long port = strtol(app.pf.port, NULL, 10);
    if (port < 1 || port > 65535)   return "bad port";

    app.pf.draft.port = (uint16_t)port;
    if (app.pf.draft.auth == STORAGE_AUTH_KEY) {
        if (!app.pf.draft.key_id[0]) return "no key - Import adds keys";
    } else {
        app.pf.draft.key_id[0] = '\0';
    }

    /* Load the authoritative on-flash set (app.profiles may hold the synth
     * "(default)" fallback, which is NOT on flash), mutate, persist. */
    conn_profile_t set[MAX_PROFILES];
    int n = 0;
    if (storage_load_profiles(set, &n, MAX_PROFILES - 1) != ESP_OK) n = 0;

    int slot = -1;                        /* slot being replaced (edit) */
    if (app.pf.edit_idx >= 0) {
        for (int i = 0; i < n; i++)
            if (strcmp(set[i].name, app.pf.orig_name) == 0) { slot = i; break; }
        if (slot < 0) return "original profile is gone";
    }
    /* Names are the profile identity everywhere (find/replace/import) —
     * a duplicate would be ambiguous to connect to and to delete. */
    for (int i = 0; i < n; i++)
        if (i != slot && strcmp(set[i].name, app.pf.draft.name) == 0)
            return "name already in use";

    conn_profile_t old = { 0 };
    if (slot < 0) {
        if (n >= MAX_PROFILES - 1) return "profile list full";
        slot = n++;
    } else {
        old = set[slot];
    }
    set[slot] = app.pf.draft;
    if (storage_save_profiles(set, n) != ESP_OK) return "save failed";

    /* The edit dropped or swapped a key reference: GC the old .pem when
     * nothing references it anymore (mirrors delete_profile_at). */
    if (old.auth == STORAGE_AUTH_KEY && old.key_id[0]) {
        bool shared = false;
        for (int i = 0; i < n; i++)
            if (set[i].auth == STORAGE_AUTH_KEY &&
                strcmp(set[i].key_id, old.key_id) == 0) { shared = true; break; }
        if (!shared) storage_delete_key(old.key_id);
    }
    return "";
}

/* Focus a field directly (must be present); caret goes to its text's end. */
static void pf_focus(int field)
{
    if (field < 0) field = PF_COUNT - 1;
    if (field >= PF_COUNT) field = 0;
    app.pf.field = field;
    if (pf_is_text(field)) {
        const char *label; int max; bool numeric, mask;
        char *buf = pf_buf(field, &label, &max, &numeric, &mask);
        app.pf.cursor = (int)strlen(buf);
    }
}

/* Step the focus ring by @p dir, skipping fields the auth mode hides. */
static void pf_focus_step(int dir)
{
    int f = app.pf.field;
    do {
        f += dir;
        if (f < 0) f = PF_COUNT - 1;
        if (f >= PF_COUNT) f = 0;
    } while (!pf_field_present(f));
    pf_focus(f);
}

/* Leave the profile editor for wherever it was entered from. */
static void exit_profile(uint64_t now, bool saved)
{
    if (app.pf.return_menu) {
        app.state = ST_MENU;
        menu_goto(MS_PROFILES);
        if (saved) menu_note(now, MENU_MSG_MS, false, "profile saved");
    } else {
        if (saved) toast(now, "profile saved");
        enter_home(now);
    }
}

void profile_tick(uint64_t now)
{
    if (now >= app.next_anim) {   /* titlebar spark + comet + caret life */
        app.next_anim = now + ANIM_PERIOD_MS;
        render_profile();
    }
}

void profile_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    if (k == K_ESC) { exit_profile(now, false); return; }

    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);   /* Save/Cancel tiles */
        if (slot == 0) {
            pf_focus(PF_SAVE);
            k = K_ENTER;   /* fall through into Save activation below */
        } else if (slot == 1) {
            exit_profile(now, false);
            return;
        } else {
            /* Tap on a form row: focus it (selectors also step). */
            int row = ev->y / font_height(), cc = ev->x / font_width();
            int f = -1;
            if (cc >= pf_x0() - 1 && cc <= pf_fx() + pf_fw() &&
                row >= pf_y0() && row < pf_y0() + PF_ROWS * pf_step() &&
                (row - pf_y0()) % pf_step() == 0)
                f = (row - pf_y0()) / pf_step();
            if (f < 0 || f >= PF_ROWS || !pf_field_present(f)) return;
            if (f == PF_AUTH)     pf_auth_toggle();
            else if (f == PF_KEY) pf_key_cycle(+1);
            app.pf.err[0] = '\0';
            pf_focus(f);
            render_profile();
            return;
        }
    }

    /* ---- button focus (Save / Cancel) ---- */
    if (app.pf.field >= PF_SAVE) {
        switch (k) {
        case K_LEFT:  pf_focus(PF_SAVE);   render_profile(); break;
        case K_RIGHT: pf_focus(PF_CANCEL); render_profile(); break;
        case K_UP:                              /* backward through ring */
            pf_focus_step(-1); render_profile(); break;
        case K_DOWN: case K_TAB:                /* forward (Cancel wraps) */
            pf_focus_step(+1); render_profile(); break;
        case K_ENTER:
            if (app.pf.field == PF_CANCEL) { exit_profile(now, false); break; }
            else {
                const char *err = profile_commit();
                if (err[0]) {   /* inline — a modal has no toast strip */
                    snprintf(app.pf.err, sizeof(app.pf.err), "%s", err);
                    render_profile();
                } else {
                    load_profiles();
                    exit_profile(now, true);
                }
            }
            break;
        default: break;
        }
        return;
    }

    /* ---- selector rows (auth toggle / key picker) ---- */
    if (app.pf.field == PF_AUTH || app.pf.field == PF_KEY) {
        switch (k) {
        case K_LEFT: case K_RIGHT:
            if (app.pf.field == PF_AUTH) pf_auth_toggle();
            else pf_key_cycle(k == K_RIGHT ? +1 : -1);
            app.pf.err[0] = '\0';
            render_profile();
            break;
        case K_CHAR:
            if (ch != ' ') break;               /* space also steps it */
            if (app.pf.field == PF_AUTH) pf_auth_toggle();
            else pf_key_cycle(+1);
            app.pf.err[0] = '\0';
            render_profile();
            break;
        case K_UP:    pf_focus_step(-1); render_profile(); break;
        case K_DOWN: case K_TAB:
        case K_ENTER: pf_focus_step(+1); render_profile(); break;
        default: break;
        }
        return;
    }

    /* ---- text field editing ---- */
    const char *label; int max; bool numeric, mask;
    char *buf = pf_buf(app.pf.field, &label, &max, &numeric, &mask);
    int len = (int)strlen(buf);
    switch (k) {
    case K_CHAR:
        if (numeric && !(ch >= '0' && ch <= '9')) break;
        /* Section-header metacharacters corrupt profiles.ini on reload;
         * block them at the source. */
        if (app.pf.field == PF_NAME && (ch == '[' || ch == ']')) break;
        if (len < max) {
            memmove(buf + app.pf.cursor + 1, buf + app.pf.cursor, len - app.pf.cursor + 1);
            buf[app.pf.cursor++] = ch;
            app.pf.err[0] = '\0';              /* an edit clears the error */
            render_profile();
        }
        break;
    case K_BACKSPACE:
        if (app.pf.cursor > 0) {
            memmove(buf + app.pf.cursor - 1, buf + app.pf.cursor, len - app.pf.cursor + 1);
            app.pf.cursor--;
            app.pf.err[0] = '\0';
            render_profile();
        }
        break;
    case K_LEFT:  if (app.pf.cursor > 0)   { app.pf.cursor--; render_profile(); } break;
    case K_RIGHT: if (app.pf.cursor < len) { app.pf.cursor++; render_profile(); } break;
    case K_UP:    pf_focus_step(-1); render_profile(); break;
    case K_DOWN: case K_TAB:
    case K_ENTER: pf_focus_step(+1); render_profile(); break;
    default: break;
    }
}
