/*
 * app_profile.c — on-device profile editor (ST_PROFILE): text fields,
 * auth-mode toggle, stored-key picker, validation + persist.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "app_menu_defs.h"   /* the editor exits back into MS_PROFILES */
#include "font.h"            /* form-row touch mapping */
#include "keystore.h"        /* keystore_wipe */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"   /* key-id list lives in SPIRAM */

static struct {
    conn_profile_t draft;           /* the profile the user is entering */
    char     port[6];               /* port as text (parsed on save)    */
    int      field;                 /* focused field (pf_field_t)       */
    int      cursor;                /* caret within the focused field   */
    char     err[40];               /* inline validation error, "" = ok */
    int      edit_idx;              /* index the user is editing, -1 = new */
    /* orig_name holds the name at edit entry. save() re-finds the slot
     * by name, since profiles may reorder meanwhile. */
    char     orig_name[32];
    char   (*keys)[STORAGE_KEY_ID_LEN];  /* SPIRAM, PF_KEY_MAX entries  */
    int      nkeys;
    int      key_sel;               /* index into keys, -1 = none       */
    char     key_type[24];          /* cached type of the selected key  */
} s_pf = { .edit_idx = -1, .key_sel = -1 };

void profile_creds_wipe(void)
{
    keystore_wipe(s_pf.draft.password, sizeof(s_pf.draft.password));
}

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
    return f != PF_KEY || s_pf.draft.auth == STORAGE_AUTH_KEY;
}

/* Resolve a text field's label, buffer, max length and flags. */
static char *pf_buf(int i, const char **label, int *max,
                    bool *numeric, bool *mask)
{
    *numeric = false; *mask = false;
    switch (i) {
    case PF_NAME: *label = "Name"; *max = sizeof(s_pf.draft.name) - 1;
                  return s_pf.draft.name;
    case PF_HOST: *label = "Host"; *max = sizeof(s_pf.draft.host) - 1;
                  return s_pf.draft.host;
    case PF_PORT: *label = "Port"; *max = 5; *numeric = true;
                  return s_pf.port;
    case PF_USER: *label = "User"; *max = sizeof(s_pf.draft.user) - 1;
                  return s_pf.draft.user;
    case PF_PASS: /* doubles as the key passphrase under key auth */
                  *label = s_pf.draft.auth == STORAGE_AUTH_KEY
                           ? "Phrase" : "Pass";
                  *max = sizeof(s_pf.draft.password) - 1;
                  *mask = true; return s_pf.draft.password;
    }
    *label = ""; *max = 0; return NULL;
}

/* (Re)load the key picker's id list and cached type of the selection. */
static void pf_key_refresh_info(void)
{
    s_pf.key_type[0] = '\0';
    if (s_pf.key_sel >= 0 && s_pf.key_sel < s_pf.nkeys)
        storage_key_info(s_pf.keys[s_pf.key_sel],
                         s_pf.key_type, sizeof(s_pf.key_type), NULL, 0);
}

static void pf_load_keys(void)
{
    if (!s_pf.keys)
        s_pf.keys = heap_caps_malloc(PF_KEY_MAX * STORAGE_KEY_ID_LEN,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_pf.nkeys = 0;
    if (s_pf.keys)
        storage_list_keys(s_pf.keys, PF_KEY_MAX, &s_pf.nkeys);

    /* Preselect the draft's key when editing. A draft without one starts
     * on the first stored key. It adopts that key only if the user
     * switches auth to key. */
    s_pf.key_sel = -1;
    for (int i = 0; i < s_pf.nkeys; i++)
        if (strcmp(s_pf.keys[i], s_pf.draft.key_id) == 0) {
            s_pf.key_sel = i;
            break;
        }
    if (s_pf.key_sel < 0 && !s_pf.draft.key_id[0] && s_pf.nkeys > 0)
        s_pf.key_sel = 0;
    pf_key_refresh_info();
}

static void pf_auth_toggle(void)
{
    if (s_pf.draft.auth == STORAGE_AUTH_KEY) {
        s_pf.draft.auth = STORAGE_AUTH_PASSWORD;
    } else {
        s_pf.draft.auth = STORAGE_AUTH_KEY;
        if (s_pf.key_sel >= 0 && s_pf.key_sel < s_pf.nkeys)
            snprintf(s_pf.draft.key_id, sizeof(s_pf.draft.key_id), "%s",
                     s_pf.keys[s_pf.key_sel]);
    }
}

static void pf_key_cycle(int dir)
{
    if (s_pf.nkeys <= 0) return;
    s_pf.key_sel = (s_pf.key_sel < 0)
              ? (dir > 0 ? 0 : s_pf.nkeys - 1)
              : (s_pf.key_sel + dir + s_pf.nkeys) % s_pf.nkeys;
    snprintf(s_pf.draft.key_id, sizeof(s_pf.draft.key_id), "%s", s_pf.keys[s_pf.key_sel]);
    pf_key_refresh_info();
}

/* Form geometry derives from the grid, so the editor fits sizes from
 * 100x30 down to 66x20. Wide grids center the form. Narrow ones hug
 * the left edge and drop to single-row field spacing. The touch
 * hit-test shares these values. */
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

static void render_profile(uint64_t now)
{
    (void)now;
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_screen_header(s_pf.edit_idx >= 0 ? "EDIT PROFILE" : "NEW PROFILE",
                       "// SSH DECK");

    for (int i = 0; i < PF_ROWS; i++) {
        int row = pf_y0() + i * pf_step();
        bool focused = (s_pf.field == i);
        if (pf_is_text(i)) {
            const char *label; int max; bool numeric, mask;
            char *buf = pf_buf(i, &label, &max, &numeric, &mask);
            ui_pen(focused ? OVERLAY_COL_CYAN : OVERLAY_COL_DEFAULT);
            ui_puts(pf_x0(), row, label, 0);
            /* Only the focused field's caret drives scrolling. */
            ui_field(pf_fx(), row, pf_fw(), buf, focused ? s_pf.cursor : 0,
                     focused, mask);
        } else if (i == PF_AUTH) {
            ui_pen(focused ? OVERLAY_COL_CYAN : OVERLAY_COL_DEFAULT);
            ui_puts(pf_x0(), row, "Auth", 0);
            pf_draw_selector(row, focused,
                             s_pf.draft.auth == STORAGE_AUTH_KEY
                             ? "key" : "password");
        } else if (i == PF_KEY && s_pf.draft.auth == STORAGE_AUTH_KEY) {
            ui_pen(focused ? OVERLAY_COL_CYAN : OVERLAY_COL_DEFAULT);
            ui_puts(pf_x0(), row, "Key", 0);
            char v[64];
            if (s_pf.key_sel >= 0 && s_pf.key_sel < s_pf.nkeys)
                snprintf(v, sizeof(v), "%s%s%s", s_pf.keys[s_pf.key_sel],
                         s_pf.key_type[0] ? "  " : "", s_pf.key_type);
            else if (s_pf.draft.key_id[0])
                snprintf(v, sizeof(v), "%s (missing)", s_pf.draft.key_id);
            else
                snprintf(v, sizeof(v), "(no keys - use Import)");
            pf_draw_selector(row, focused, v);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    /* Inline validation error — a modal has no toast strip, so a failed
     * Save must report here or it looks dead. */
    if (s_pf.err[0]) {
        ui_pen(OVERLAY_COL_RED);
        ui_puts(pf_x0(), pf_y0() + PF_ROWS * pf_step(), s_pf.err, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* Save / Cancel buttons — the bar grid stashed for touch. */
    tilegrid_t bg = ui_button_bar(pf_y0() + PF_ROWS * pf_step() + 1, 2, 20,
                                  ui_rows() >= 28 ? 3 : 2);
    app.grid = bg;
    ui_pen(OVERLAY_COL_GREEN);
    ui_button(&bg, 0, "Save", "", s_pf.field == PF_SAVE);
    ui_pen(OVERLAY_COL_BLUE);   /* safe navigation — matches menu Back */
    ui_button(&bg, 1, "Cancel", "", s_pf.field == PF_CANCEL);
    ui_pen(OVERLAY_COL_DEFAULT);

}

/* @p arg = stored profile to edit, or -1 for a new one. */
static void profile_enter(intptr_t arg, uint64_t now)
{
    int edit_idx = (int)arg;
    (void)now;
    memset(&s_pf.draft, 0, sizeof(s_pf.draft));
    s_pf.edit_idx     = (edit_idx >= 0 && edit_idx < app.stored_count)
                   ? edit_idx : -1;
    s_pf.orig_name[0] = '\0';
    if (s_pf.edit_idx >= 0) {
        s_pf.draft = app.profiles[s_pf.edit_idx];
        snprintf(s_pf.orig_name, sizeof(s_pf.orig_name), "%s", s_pf.draft.name);
        snprintf(s_pf.port, sizeof(s_pf.port), "%u", (unsigned)s_pf.draft.port);
    } else {
        snprintf(s_pf.port, sizeof(s_pf.port), "22");
    }
    pf_load_keys();
    s_pf.field  = PF_NAME;
    s_pf.cursor = (int)strlen(s_pf.draft.name);
    s_pf.err[0] = '\0';
}

void enter_profile(uint64_t now, int edit_idx)
{
    nav_push(SCR_PROFILE, edit_idx, now);
}

/* Validate the draft and persist it. Append for a new profile; replace
 * the original (found by its entry-time name) for an edit. Returns ""
 * on success or a short reason to show inline on failure. */
static const char *profile_commit(void)
{
    if (s_pf.draft.name[0] == '\0') return "name required";
    /* A '[' or ']' in the name breaks the INI section header on save. It
     * also silently corrupts the file on reload. Reject them. */
    if (strpbrk(s_pf.draft.name, "[]")) return "name: no [ or ]";
    if (s_pf.draft.host[0] == '\0') return "host required";
    if (s_pf.draft.user[0] == '\0') return "user required";
    long port = strtol(s_pf.port, NULL, 10);
    if (port < 1 || port > 65535)   return "bad port";

    s_pf.draft.port = (uint16_t)port;
    if (s_pf.draft.auth == STORAGE_AUTH_KEY) {
        if (!s_pf.draft.key_id[0]) return "no key - Import adds keys";
    } else {
        s_pf.draft.key_id[0] = '\0';
    }

    /* Load the authoritative on-flash set (app.profiles may hold the synth
     * "(default)" fallback, which is NOT on flash), mutate, persist. */
    conn_profile_t set[MAX_PROFILES];
    int n = 0;
    if (storage_load_profiles(set, &n, MAX_PROFILES - 1) != ESP_OK) n = 0;

    int slot = -1;                        /* slot to overwrite, when editing */
    if (s_pf.edit_idx >= 0) {
        for (int i = 0; i < n; i++)
            if (strcmp(set[i].name, s_pf.orig_name) == 0) { slot = i; break; }
        if (slot < 0) return "original profile is gone";
    }
    /* Names are the profile identity everywhere (find/replace/import) —
     * a duplicate would be ambiguous to connect to and to delete. */
    for (int i = 0; i < n; i++)
        if (i != slot && strcmp(set[i].name, s_pf.draft.name) == 0)
            return "name already in use";

    conn_profile_t old = { 0 };
    if (slot < 0) {
        if (n >= MAX_PROFILES - 1) return "profile list full";
        slot = n++;
    } else {
        old = set[slot];
    }
    set[slot] = s_pf.draft;
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
    s_pf.field = field;
    if (pf_is_text(field)) {
        const char *label; int max; bool numeric, mask;
        char *buf = pf_buf(field, &label, &max, &numeric, &mask);
        s_pf.cursor = (int)strlen(buf);
    }
}

/* Step the focus ring by @p dir, skipping fields the auth mode hides. */
static void pf_focus_step(int dir)
{
    int f = s_pf.field;
    do {
        f += dir;
        if (f < 0) f = PF_COUNT - 1;
        if (f >= PF_COUNT) f = 0;
    } while (!pf_field_present(f));
    pf_focus(f);
}

/* Leave the editor: the stack remembers where the caller opened it from. */
static void exit_profile(uint64_t now, bool saved)
{
    nav_pop(now);
    if (saved) {
        if (nav_current() == SCR_MENU)
            menu_note(now, MENU_MSG_MS, false, "profile saved");
        else
            toast(now, "profile saved");
    }
}

static void profile_tick(uint64_t now)
{
    if (now >= app.next_anim) {   /* titlebar spark + comet + caret life */
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

static void profile_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                          uint64_t now)
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
            s_pf.err[0] = '\0';
            pf_focus(f);
            nav_invalidate();
            return;
        }
    }

    /* ---- button focus (Save / Cancel) ---- */
    if (s_pf.field >= PF_SAVE) {
        switch (k) {
        case K_LEFT:  pf_focus(PF_SAVE);   nav_invalidate(); break;
        case K_RIGHT: pf_focus(PF_CANCEL); nav_invalidate(); break;
        case K_UP:                              /* backward through ring */
            pf_focus_step(-1); nav_invalidate(); break;
        case K_DOWN: case K_TAB:                /* forward, wraps past Cancel */
            pf_focus_step(+1); nav_invalidate(); break;
        case K_ENTER:
            if (s_pf.field == PF_CANCEL) { exit_profile(now, false); break; }
            else {
                const char *err = profile_commit();
                if (err[0]) {   /* inline — a modal has no toast strip */
                    snprintf(s_pf.err, sizeof(s_pf.err), "%s", err);
                    nav_invalidate();
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
    if (s_pf.field == PF_AUTH || s_pf.field == PF_KEY) {
        switch (k) {
        case K_LEFT: case K_RIGHT:
            if (s_pf.field == PF_AUTH) pf_auth_toggle();
            else pf_key_cycle(k == K_RIGHT ? +1 : -1);
            s_pf.err[0] = '\0';
            nav_invalidate();
            break;
        case K_CHAR:
            if (ch != ' ') break;               /* space also steps it */
            if (s_pf.field == PF_AUTH) pf_auth_toggle();
            else pf_key_cycle(+1);
            s_pf.err[0] = '\0';
            nav_invalidate();
            break;
        case K_UP:    pf_focus_step(-1); nav_invalidate(); break;
        case K_DOWN: case K_TAB:
        case K_ENTER: pf_focus_step(+1); nav_invalidate(); break;
        default: break;
        }
        return;
    }

    /* ---- text field editing ---- */
    const char *label; int max; bool numeric, mask;
    char *buf = pf_buf(s_pf.field, &label, &max, &numeric, &mask);
    int len = (int)strlen(buf);
    switch (k) {
    case K_CHAR:
        if (numeric && !(ch >= '0' && ch <= '9')) break;
        /* Section-header metacharacters corrupt profiles.ini on reload;
         * block them at the source. */
        if (s_pf.field == PF_NAME && (ch == '[' || ch == ']')) break;
        if (len < max) {
            memmove(buf + s_pf.cursor + 1, buf + s_pf.cursor, len - s_pf.cursor + 1);
            buf[s_pf.cursor++] = ch;
            s_pf.err[0] = '\0';              /* an edit clears the error */
            nav_invalidate();
        }
        break;
    case K_BACKSPACE:
        if (s_pf.cursor > 0) {
            memmove(buf + s_pf.cursor - 1, buf + s_pf.cursor, len - s_pf.cursor + 1);
            s_pf.cursor--;
            s_pf.err[0] = '\0';
            nav_invalidate();
        }
        break;
    case K_LEFT:  if (s_pf.cursor > 0)   { s_pf.cursor--; nav_invalidate(); } break;
    case K_RIGHT: if (s_pf.cursor < len) { s_pf.cursor++; nav_invalidate(); } break;
    case K_UP:    pf_focus_step(-1); nav_invalidate(); break;
    case K_DOWN: case K_TAB:
    case K_ENTER: pf_focus_step(+1); nav_invalidate(); break;
    default: break;
    }
}

const nav_screen_t profile_screen = {
    .name = "profile", .enter = profile_enter, .tick = profile_tick,
    .input = profile_input, .render = render_profile,
    .chrome = NAV_CHROME_FULL,
};
