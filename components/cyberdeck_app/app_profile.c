/*
 * app_profile.c - the host form (SCR_PROFILE): new host or edit of a stored
 * one, on the slate parts. Design: docs/ui-kit.md; reference drawing:
 * draw_edit in sim/uilab_mo.c.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "app_form.h"
#include "keystore.h"        /* keystore_wipe, keystore_state */
#include "ui_pure.h"         /* caret window and text edits */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"   /* key-id list lives in SPIRAM */
#include "esp_log.h"

/* Entries first (ids 1-5), then the other parts. Tab walks them in order. */
enum {
    ID_NAME = 1, ID_USER, ID_HOST, ID_PORT, ID_PASS,
    ID_KEY, ID_SHOW, ID_SAVE,
    ID_AUTH,                        /* chips: ID_AUTH + 0 password, + 1 key */
};

static const char *TAG = "app_profile";

#define PF_KEY_MAX 8              /* key ids offered by the picker */

static const app_form_def_t PF_FORM;

static struct {
    conn_profile_t draft;           /* the profile the user is entering */
    char     port[6];               /* port as text (parsed on save)    */
    app_form_t form;                /* typing state: edit, show, caret  */
    bool     dirty;                 /* any change since entry           */
    ui_id_t  saved_focus;          /* focus saved when a page opens    */
    char     err[40];               /* commit error, "" = none          */
    int      edit_idx;              /* index the user is editing, -1 = new */
    /* orig_name holds the name at edit entry. save() re-finds the slot
     * by name, since profiles may reorder meanwhile. */
    char     orig_name[32];
    char   (*keys)[STORAGE_KEY_ID_LEN];  /* SPIRAM, PF_KEY_MAX entries  */
    int      nkeys;
    int      key_sel;               /* index into keys, -1 = none       */
    char     key_type[24];          /* cached type of the selected key  */
} s_pf = { .edit_idx = -1, .key_sel = -1, .form = { .def = &PF_FORM } };

void profile_creds_wipe(void)
{
    keystore_wipe(s_pf.draft.password, sizeof(s_pf.draft.password));
}

static bool pf_key_auth(void)
{
    return s_pf.draft.auth == STORAGE_AUTH_KEY;
}

/* The text buffer and length limit behind an entry id. */
static char *pf_buf(ui_id_t id, int *max)
{
    switch (id) {
    case ID_NAME: *max = sizeof(s_pf.draft.name) - 1;     return s_pf.draft.name;
    case ID_USER: *max = sizeof(s_pf.draft.user) - 1;     return s_pf.draft.user;
    case ID_HOST: *max = sizeof(s_pf.draft.host) - 1;     return s_pf.draft.host;
    case ID_PORT: *max = 5;                               return s_pf.port;
    case ID_PASS: *max = sizeof(s_pf.draft.password) - 1; return s_pf.draft.password;
    default:      *max = 0;                               return NULL;
    }
}

/* Name refuses [ and ]: they break the INI section header. */
static bool pf_accepts(ui_id_t id, char ch)
{
    if (ch < 0x20 || ch > 0x7E) return false;
    if (id == ID_PORT) return ch >= '0' && ch <= '9';
    if (id == ID_NAME) return ch != '[' && ch != ']';
    return true;
}

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
     * switches sign-in to key. */
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

/* Right text of the Key part: id, type and place in the list, as far as
 * @p cap cells allow. */
static void pf_key_text(char *out, size_t sz, int cap)
{
    const bool known = s_pf.key_sel >= 0 && s_pf.key_sel < s_pf.nkeys;
    if (!known) {
        if (s_pf.draft.key_id[0])
            snprintf(out, sz, "%.*s (missing)", cap - 10, s_pf.draft.key_id);
        else
            snprintf(out, sz, "none stored");
        return;
    }
    const char *id = s_pf.keys[s_pf.key_sel];
    char pos[24] = "";
    if (s_pf.nkeys > 1)
        snprintf(pos, sizeof pos, " %d/%d", s_pf.key_sel + 1, s_pf.nkeys);
    if (s_pf.key_type[0] &&
        (int)(strlen(id) + strlen(s_pf.key_type) + strlen(pos)) + 2 <= cap)
        snprintf(out, sz, "%s  %s%s", id, s_pf.key_type, pos);
    else
        snprintf(out, sz, "%.*s%s", cap - (int)strlen(pos), id, pos);
}

typedef struct { bool port, name, host, user, key; } pf_bad_t;

static bool pf_port_ok(void)
{
    if (!s_pf.port[0]) return false;
    const long v = strtol(s_pf.port, NULL, 10);
    return v >= 1 && v <= 65535;
}

static pf_bad_t pf_bad(void)
{
    return (pf_bad_t){
        .port = !pf_port_ok(),
        .name = !s_pf.draft.name[0],
        .host = !s_pf.draft.host[0],
        .user = !s_pf.draft.user[0],
        .key  = pf_key_auth() && !s_pf.draft.key_id[0],
    };
}

static bool pf_wrong(const pf_bad_t *b)
{
    return b->port || b->name || b->host || b->user || b->key;
}

/* The alert line: a commit error, else the first rule that fails. A new
 * form shows no red for empty fields until something changed. */
static const char *pf_alert(const pf_bad_t *b)
{
    if (s_pf.err[0])                   return s_pf.err;
    if (b->port)                       return "Port: a number from 1 to 65535.";
    if (b->name && s_pf.dirty)         return "A host needs a name.";
    if (b->host && s_pf.dirty)         return "A host needs an address.";
    if (b->user && s_pf.dirty)         return "A host needs a user.";
    if (b->key)         return "No stored key. Setup, Hosts imports one.";
    return NULL;
}

static void pf_entry(ui_rect_t r, ui_id_t id, const char *label, int label_w,
                     bool bad, bool secret)
{
    app_form_entry(&s_pf.form, r, id, label, label_w, bad, secret);
}

static void render_profile(uint64_t now)
{
    (void)now;
    const int faults = ui_faults();
    const ui_slate_t pg = ui_slate_page();
    static const char *const AUTH[2] = { "password", "key" };
    const ui_rect_t body = pg.body;
    const int gx = pg.m.gutter;

    ui_focus_default(ID_NAME);
    const pf_bad_t bad = pf_bad();
    const bool invalid = pf_wrong(&bad);
    /* Empty fields show red only once the form changed. */
    const bool show_name = bad.name && s_pf.dirty;
    const bool show_host = bad.host && s_pf.dirty;
    const bool show_user = bad.user && s_pf.dirty;

    char title[64];
    if (s_pf.edit_idx >= 0)
        snprintf(title, sizeof title, "Edit %s", s_pf.orig_name);
    else
        snprintf(title, sizeof title, "New host");
    ui_rect_t head = pg.header;
    ui_pill(&head, ID_SAVE, "Save", UI_TONE_SOLID, OVERLAY_COL_GREEN,
            invalid ? UI_PART_OFF : 0);
    ui_title(head, "Hosts", title, NULL, true);

    const int half = (body.w - gx) >> 1, port_w = 17;
    const int wide = body.w - gx - port_w;
    pf_entry(ui_rect(body.x, body.y, half, 3), ID_NAME, "Name", 0,
             show_name, false);
    pf_entry(ui_rect(body.x + half + gx, body.y, body.w - half - gx, 3),
             ID_USER, "User", 0, show_user, false);
    pf_entry(ui_rect(body.x, body.y + 3, wide, 3), ID_HOST, "Host", 0,
             show_host, false);
    pf_entry(ui_rect(body.x + wide + gx, body.y + 3, port_w, 3), ID_PORT,
             "Port", 6, bad.port, false);
    int y = body.y + 6;
    ui_chips(ui_rect(body.x, y, body.w, 3), ID_AUTH, "Sign-in", AUTH, 2,
             pf_key_auth() ? 1 : 0, 0, 12);
    y += 3;

    const bool key = pf_key_auth();
    if (key) {
        char right[96];
        pf_key_text(right, sizeof right, body.w - 12);
        ui_part(ui_rect(body.x, y, ui_act_w("Key", right), 3), ID_KEY,
                &(ui_part_t){ .title = "Key", .right = right,
                              .flags = (uint8_t)(UI_PART_RIGHT_BOLD |
                                  UI_PART_ROOMY |
                                  (s_pf.nkeys ? 0 : UI_PART_OFF)) },
                NULL);
        y += 3;
    }
    const char *secret = key ? "Passphrase" : "Password";
    const char *toggle = key ? "Show passphrase" : "Show password";
    pf_entry(ui_rect(body.x, y, half, 3), ID_PASS, secret, key ? 12 : 0,
             false, true);
    ui_switch(ui_rect(body.x + half + gx, y, ui_switch_w(toggle), 3), ID_SHOW,
              toggle, s_pf.form.show, UI_PART_ROOMY);
    y += 3;

    const char *alert = pf_alert(&bad);
    if (alert && y < body.y + body.h) {
        ui_pen(OVERLAY_COL_RED);
        ui_text(ui_rect(body.x + 2, y, body.w - 4, 1), alert, UI_TEXT | UI_BOLD,
                UI_LEFT);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
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
    app_form_reset(&s_pf.form);
    s_pf.dirty = false;
    s_pf.saved_focus = 0;
    s_pf.err[0] = '\0';
}

/* Back from the question page: the draft is intact, only focus returns. */
static void profile_resume(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    if (s_pf.saved_focus) ui_focus_set(s_pf.saved_focus);
}

static void profile_exit(uint64_t now)
{
    (void)now;
    s_pf.saved_focus = ui_focus_get();
}

void enter_profile(uint64_t now, int edit_idx)
{
    nav_push(SCR_PROFILE, edit_idx, now);
}

/* Validate the draft and persist it. Append for a new profile; replace
 * the original (found by its entry-time name) for an edit. Returns ""
 * on success or a short reason to show on the alert line on failure. */
static const char *profile_commit(bool drop_key)
{
    if (s_pf.draft.name[0] == '\0') return "A host needs a name.";
    /* A '[' or ']' in the name breaks the INI section header on save. It
     * also silently corrupts the file on reload. Reject them. */
    if (strpbrk(s_pf.draft.name, "[]")) return "A name cannot hold [ or ].";
    if (s_pf.draft.host[0] == '\0') return "A host needs an address.";
    if (s_pf.draft.user[0] == '\0') return "A host needs a user.";
    long port = strtol(s_pf.port, NULL, 10);
    if (port < 1 || port > 65535)   return "Port: a number from 1 to 65535.";

    s_pf.draft.port = (uint16_t)port;
    if (s_pf.draft.auth == STORAGE_AUTH_KEY) {
        if (!s_pf.draft.key_id[0]) return "No stored key.";
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
        if (slot < 0) return "This host is gone.";
    }
    /* Names are the profile identity everywhere (find/replace/import) -
     * a duplicate would be ambiguous to connect to and to delete. */
    for (int i = 0; i < n; i++)
        if (i != slot && strcmp(set[i].name, s_pf.draft.name) == 0)
            return "That name is in use.";

    /* A locked store keeps its passwords in the bundle and lists a marker.
     * Any other draft password would replace the marker and orphan them. */
    if (slot >= 0 && keystore_state() == KEYSTORE_LOCKED &&
        strcmp(set[slot].password, STORAGE_PW_BUNDLED) == 0 &&
        strcmp(s_pf.draft.password, STORAGE_PW_BUNDLED) != 0)
        return "Unlock the deck first.";

    conn_profile_t old = { 0 };
    if (slot < 0) {
        if (n >= MAX_PROFILES - 1) return "The host list is full.";
        slot = n++;
    } else {
        old = set[slot];
    }
    set[slot] = s_pf.draft;
    if (storage_save_profiles(set, n) != ESP_OK) return "Could not save.";

    /* The edit dropped or swapped a key, and the user chose to delete it.
     * The old key goes when nothing references it anymore. */
    if (drop_key && old.auth == STORAGE_AUTH_KEY && old.key_id[0]) {
        bool shared = false;
        for (int i = 0; i < n; i++)
            if (set[i].auth == STORAGE_AUTH_KEY &&
                strcmp(set[i].key_id, old.key_id) == 0) { shared = true; break; }
        if (!shared) storage_delete_key(old.key_id);
    }
    return "";
}

/* Leave the form for good: the draft secret goes first. */
static void pf_leave(uint64_t now)
{
    profile_creds_wipe();
    nav_pop(now);
}

static void pf_discard_yes(intptr_t arg, uint64_t now)
{
    (void)arg;
    pf_leave(now);
}

/* A changed form asks first. Cancel there comes back to the draft. */
static void pf_back(uint64_t now)
{
    s_pf.form.edit = false;
    if (!s_pf.dirty) {
        pf_leave(now);
        return;
    }
    ask_open(&(ask_t){
        .title = "Discard the changes?",
        .line1 = s_pf.edit_idx >= 0 ? "This host keeps its old values."
                                    : "The new host is not stored.",
        .verb = "Discard", .accent = OVERLAY_COL_AMBER,
        .yes = pf_discard_yes,
    }, now);
}

static void pf_store(uint64_t now, bool drop_key)
{
    const char *err = profile_commit(drop_key);
    if (err[0]) {
        snprintf(s_pf.err, sizeof(s_pf.err), "%s", err);
        nav_invalidate();
        return;
    }
    load_profiles();
    pf_leave(now);
    toast(now, drop_key ? "host saved, key deleted" : "host saved");
}

static void pf_keep_key(intptr_t arg, uint64_t now) { (void)arg; pf_store(now, false); }
static void pf_drop_key(intptr_t arg, uint64_t now) { (void)arg; pf_store(now, true); }

/* True when the save leaves the host's old key with no host at all. */
static bool pf_orphans_key(void)
{
    if (s_pf.edit_idx < 0 || !profile_key_unshared(s_pf.edit_idx)) return false;
    const conn_profile_t *old = &app.profiles[s_pf.edit_idx];
    return s_pf.draft.auth != STORAGE_AUTH_KEY ||
           strcmp(s_pf.draft.key_id, old->key_id) != 0;
}

/* A key with no host left is the user's to delete. Cancel is the
 * default; Keep key saves without deleting. */
static void pf_save(uint64_t now)
{
    if (!pf_orphans_key()) {
        pf_store(now, false);
        return;
    }
    char title[64];
    snprintf(title, sizeof title, "Delete key %s?",
             app.profiles[s_pf.edit_idx].key_id);
    ask_open(&(ask_t){
        .title = title,
        .line1 = "No host uses it after this save.",
        .line2 = "Deleted key files cannot come back.",
        .verb = "Delete key", .accent = OVERLAY_COL_RED,
        .yes = pf_drop_key,
        .no_verb = "Keep key", .no = pf_keep_key,
    }, now);
}

/* Any change to the draft: the form is dirty and an old commit error goes. */
static void pf_changed(void)
{
    s_pf.dirty = true;
    s_pf.err[0] = '\0';
    nav_invalidate();
}

static void pf_adopt_key(void)
{
    if (s_pf.key_sel >= 0 && s_pf.key_sel < s_pf.nkeys)
        snprintf(s_pf.draft.key_id, sizeof(s_pf.draft.key_id), "%s",
                 s_pf.keys[s_pf.key_sel]);
}

static void pf_set_auth(int auth)
{
    const int want = auth ? STORAGE_AUTH_KEY : STORAGE_AUTH_PASSWORD;
    if (s_pf.draft.auth == want) return;
    s_pf.draft.auth = (uint8_t)want;
    if (auth) pf_adopt_key();
    pf_changed();
}

/* Step to the next stored key; wraps. A missing key goes to the first. */
static void pf_key_step(void)
{
    if (s_pf.nkeys <= 0) return;
    s_pf.key_sel = (s_pf.key_sel + 1) % s_pf.nkeys;
    pf_adopt_key();
    pf_key_refresh_info();
    pf_changed();
}

static void pf_press(ui_id_t id, ui_id_t prev, uint64_t now)
{
    if (app_form_is_entry(&s_pf.form, id)) {
        app_form_press_entry(&s_pf.form, id, prev);
        return;
    }
    s_pf.form.edit = false;
    if (id == UI_ID_BACK)            pf_back(now);
    else if (id == ID_SAVE)          pf_save(now);
    else if (id == ID_AUTH)          pf_set_auth(0);
    else if (id == ID_AUTH + 1)      pf_set_auth(1);
    else if (id == ID_KEY)           pf_key_step();
    else if (id == ID_SHOW)          s_pf.form.show = !s_pf.form.show;
}

/* Tab order of the controls on screen now. Save drops out while off. */
static int pf_tab_order(ui_id_t *o)
{
    int n = 0;
    const pf_bad_t bad = pf_bad();
    o[n++] = ID_NAME; o[n++] = ID_USER; o[n++] = ID_HOST; o[n++] = ID_PORT;
    o[n++] = (ui_id_t)(ID_AUTH + (pf_key_auth() ? 1 : 0));
    if (pf_key_auth() && s_pf.nkeys > 0) o[n++] = ID_KEY;
    o[n++] = ID_PASS;
    o[n++] = ID_SHOW;
    if (!pf_wrong(&bad)) o[n++] = ID_SAVE;
    return n;
}

/* The AUTH chips share one tab stop: the selected chip. */
static ui_id_t pf_tab_cur(ui_id_t cur)
{
    if (cur == ID_AUTH || cur == ID_AUTH + 1)
        return (ui_id_t)(ID_AUTH + (pf_key_auth() ? 1 : 0));
    return cur;
}

static const app_form_def_t PF_FORM = {
    .buf = pf_buf, .accepts = pf_accepts, .tab_order = pf_tab_order,
    .tab_cur = pf_tab_cur, .changed = pf_changed,
};

static void profile_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                          uint64_t now)
{
    if (k != K_NONE && s_pf.form.edit) {
        app_form_typing_key(&s_pf.form, k, ch);
        return;
    }
    if (k == K_ESC) { pf_back(now); return; }
    if (k == K_TAB) { app_form_tab(&s_pf.form); return; }
    if (k == K_CHAR) {                  /* a letter on an entry starts typing */
        app_form_char_start(&s_pf.form, ch);
        return;
    }

    const ui_id_t prev = ui_focus_get();
    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind == UI_ACT_NONE) return;
    nav_invalidate();
    if (a.kind == UI_ACT_PRESS) pf_press(a.id, prev, now);
}

static void profile_tick(uint64_t now)
{
    if (now >= app.next_anim) {   /* the status bar clock and the pressed flash */
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

const nav_screen_t profile_screen = {
    .name = "profile", .enter = profile_enter, .resume = profile_resume,
    .exit = profile_exit, .tick = profile_tick, .input = profile_input,
    .render = render_profile, .chrome = NAV_CHROME_FULL,
};
