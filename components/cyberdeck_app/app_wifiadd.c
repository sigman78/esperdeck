/*
 * app_wifiadd.c - the by-hand network form (SCR_WIFIADD): SSID, password
 * and a show switch, typed from the keyboard. Save goes through
 * app_wifi_add. Reference drawing: draw_wifiadd in sim/uilab_mo.c.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "app_form.h"
#include "app_wifi.h"
#include "keystore.h"        /* keystore_wipe */

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

enum { ID_SSID = 1, ID_PASS, ID_SHOW, ID_SAVE };

#define WA_PASS_MIN 8

static const char *TAG = "app_wifiadd";

static const app_form_def_t WA_FORM;

static struct {
    char       ssid[33];
    char       pass[64];            /* the one copy of the typed password */
    app_form_t form;
    ui_id_t    saved_focus;
    bool       asking;              /* the discard question is on top     */
} s_wa = { .form = { .def = &WA_FORM } };

/* The draft goes: SSID and password. */
static void wa_wipe(void)
{
    keystore_wipe(s_wa.ssid, sizeof(s_wa.ssid));
    keystore_wipe(s_wa.pass, sizeof(s_wa.pass));
}

void wifiadd_creds_wipe(void)
{
    wa_wipe();
}

static char *wa_buf(ui_id_t id, int *max)
{
    switch (id) {
    case ID_SSID: *max = sizeof(s_wa.ssid) - 1; return s_wa.ssid;
    case ID_PASS: *max = sizeof(s_wa.pass) - 1; return s_wa.pass;
    default:      *max = 0;                     return NULL;
    }
}

static bool wa_accepts(ui_id_t id, char ch)
{
    (void)id;
    return ch >= 0x20 && ch <= 0x7E;
}

/* wifi.ini drops a trailing space on load. app_wifi_list_add refuses
 * one too. */
static bool ends_in_space(const char *s)
{
    const size_t n = strlen(s);
    return n > 0 && s[n - 1] == ' ';
}

static bool wa_ssid_bad(void)
{
    return ends_in_space(s_wa.ssid);
}

static bool wa_pass_bad(void)
{
    const size_t n = strlen(s_wa.pass);
    return (n > 0 && n < WA_PASS_MIN) || ends_in_space(s_wa.pass);
}

/* The first fault on the form, or NULL. */
static const char *wa_fault(void)
{
    if (wa_ssid_bad())              return "The name cannot end in a space";
    if (ends_in_space(s_wa.pass))   return "The password cannot end in a space";
    if (wa_pass_bad())              return "Password needs 8 to 63 characters";
    return NULL;
}

static bool wa_wrong(void)
{
    return !s_wa.ssid[0] || wa_fault();
}

static bool wa_dirty(void)
{
    return s_wa.ssid[0] || s_wa.pass[0];
}

static void render_wifiadd(uint64_t now)
{
    (void)now;
    const int faults = ui_faults();
    const ui_slate_t pg = ui_slate_page();
    const ui_rect_t body = pg.body;

    ui_focus_default(ID_SSID);
    ui_rect_t head = pg.header;
    ui_pill(&head, ID_SAVE, "Save", UI_TONE_SOLID, OVERLAY_COL_GREEN,
            wa_wrong() ? UI_PART_OFF : 0);
    ui_title(head, "Wi-Fi", "Add network", NULL, true);

    /* The host form's captions; the entry stops at 40 cells, Show follows. */
    ui_form_cols_t g = ui_form_cols(body, ui_caption_w("Password"),
                                    ui_caption_w("Show"));
    const int room = body.w - g.cap_w - g.cap2_w - UI_SWITCH_W;
    g.a = room < 40 ? room : 40;
    g.x2 = g.x1 + g.a + g.cap2_w;
    int y = ui_form_row(body, &g, 0, "Network", NULL);
    app_form_entry(&s_wa.form, ui_rect(g.x1, y, g.a, 3), ID_SSID,
                   wa_ssid_bad(), false);
    y = ui_form_row(body, &g, 1, "Password", "Show");
    app_form_entry(&s_wa.form, ui_rect(g.x1, y, g.a, 3), ID_PASS,
                   wa_pass_bad(), true);
    ui_switch(ui_rect(g.x2, y, UI_SWITCH_W, 3), ID_SHOW, s_wa.form.show, 0);

    const char *fault = wa_fault();
    if (fault && body.h > 6) {
        ui_pen(OVERLAY_COL_RED);
        ui_text(ui_rect(body.x + 2, body.y + 6, body.w - 4, 1),
                fault, UI_TEXT | UI_BOLD, UI_LEFT);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

static void wifiadd_enter(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    wa_wipe();
    app_form_reset(&s_wa.form);
    s_wa.saved_focus = 0;
    s_wa.asking = false;
}

/* Back from the question: the draft is intact, only focus returns. */
static void wifiadd_resume(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    s_wa.asking = false;
    if (s_wa.saved_focus) ui_focus_set(s_wa.saved_focus);
}

/* Any departure but the question page takes the draft with it. */
static void wifiadd_exit(uint64_t now)
{
    (void)now;
    s_wa.saved_focus = ui_focus_get();
    if (!s_wa.asking) wa_wipe();
}

void enter_wifiadd(uint64_t now)
{
    nav_push(SCR_WIFIADD, 0, now);
}

static void wa_leave(uint64_t now)
{
    wa_wipe();
    nav_pop(now);
}

static void wa_discard_yes(intptr_t arg, uint64_t now)
{
    (void)arg;
    wa_leave(now);
}

/* A non-empty draft asks first. Cancel there comes back to the draft. */
static void wa_back(uint64_t now)
{
    s_wa.form.edit = false;
    if (!wa_dirty()) {
        wa_leave(now);
        return;
    }
    s_wa.asking = true;
    ask_open(&(ask_t){
        .title = "Discard the network?",
        .line1 = "It is not saved.",
        .verb = "Discard", .accent = OVERLAY_COL_AMBER,
        .yes = wa_discard_yes,
    }, now);
}

static void wa_save(uint64_t now)
{
    if (wa_wrong()) return;
    const esp_err_t e = app_wifi_add(s_wa.ssid, s_wa.pass);
    if (e == ESP_OK) {
        wa_leave(now);
        toast(now, "network saved");
        return;
    }
    toast(now, e == ESP_ERR_INVALID_STATE ? "unlock the deck first"
             : e == ESP_ERR_NO_MEM        ? "the network list is full"
                                          : "save failed");
    nav_invalidate();
}

static void wa_changed(void)
{
    nav_invalidate();
}

static void wa_press(ui_id_t id, ui_id_t prev, uint64_t now)
{
    if (app_form_is_entry(&s_wa.form, id)) {
        app_form_press_entry(&s_wa.form, id, prev);
        return;
    }
    s_wa.form.edit = false;
    if (id == UI_ID_BACK)   wa_back(now);
    else if (id == ID_SAVE) wa_save(now);
    else if (id == ID_SHOW) s_wa.form.show = !s_wa.form.show;
}

/* Save drops out of the Tab walk while it is off. */
static int wa_tab_order(ui_id_t *o)
{
    int n = 0;
    o[n++] = ID_SSID; o[n++] = ID_PASS; o[n++] = ID_SHOW;
    if (!wa_wrong()) o[n++] = ID_SAVE;
    return n;
}

static const app_form_def_t WA_FORM = {
    .buf = wa_buf, .accepts = wa_accepts, .tab_order = wa_tab_order,
    .changed = wa_changed,
};

static void wifiadd_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                          uint64_t now)
{
    if (k != K_NONE && s_wa.form.edit) {
        app_form_typing_key(&s_wa.form, k, ch);
        return;
    }
    if (k == K_ESC) { wa_back(now); return; }
    if (k == K_TAB) { app_form_tab(&s_wa.form); return; }
    if (k == K_CHAR) {
        app_form_char_start(&s_wa.form, ch);
        return;
    }

    const ui_id_t prev = ui_focus_get();
    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind == UI_ACT_NONE) return;
    nav_invalidate();
    if (a.kind == UI_ACT_PRESS) wa_press(a.id, prev, now);
}

static void wifiadd_tick(uint64_t now)
{
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

const nav_screen_t wifiadd_screen = {
    .name = "wifiadd", .enter = wifiadd_enter, .resume = wifiadd_resume,
    .exit = wifiadd_exit, .tick = wifiadd_tick, .input = wifiadd_input,
    .render = render_wifiadd, .chrome = NAV_CHROME_FULL,
};
