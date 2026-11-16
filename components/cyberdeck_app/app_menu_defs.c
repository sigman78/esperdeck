/*
 * app_menu_defs.c - the setup tree as data: section tables whose items
 * carry their own action, state and options. See app_menu_defs.h.
 * Drawing and input live in app_menu.c.
 */

#include "app_menu_defs.h"
#include "app_internal.h"
#include "app_screens.h"
#include "app_settings.h"
#include "app_widgets.h"
#include "app_wifi.h"
#include "font.h"
#include "keystore.h"
#include "ssh_client.h"
#include "vterm.h"         /* vterm_reset(): in-session terminal reset */
#include "wifi_manager.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"       /* provides CONFIG_CYBERDECK_KEYSTORE (sim: -D flag) */
#endif

#include <stdio.h>
#include <string.h>

#ifndef BUILD_SIMULATOR
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"     /* esp_restart(): applying a new font size */
#endif

#define NOT_BUILT "not in this build"

void menu_act_disconnect(uint64_t now)
{
    ssh_client_disconnect();
    enter_home_after_collapse(now);   /* deliberate CRT power-off */
}

/* Escape hatch for a wedged client state on a live link (stuck ?2026
 * hold, garbage after a dropped byte). The remote gets a SIGWINCH nudge
 * to repaint. Modes it switched on (DECCKM and friends) stay gone until
 * it re-sends them, as in every terminal's "full reset". */
void menu_act_reset_terminal(uint64_t now)
{
    vterm_reset();
    int rc = ssh_client_request_redraw();
    menu_back(now);
    toast(now, rc == 0 ? "terminal reset - remote asked to redraw"
                       : "terminal reset - remote NOT nudged");
}

static void act_import(intptr_t mode, uint64_t now)
{
    enter_sshimport(now, (ssh_import_mode_t)mode);
}

/* A live session shuts the list: the host page could change its hosts. */
static bool hosts_shut(void)
{
    return nav_on_stack(SCR_SESSION);
}

static bool off_new_host(intptr_t a)
{
    (void)a;
    return hosts_shut() || app.stored_count >= STORAGE_MAX_PROFILES;
}

static void act_new_host(intptr_t a, uint64_t now)
{
    (void)a;
    enter_profile(now, -1);
}

static const menu_item_t hosts_items[] = {
    { .label = "+ New", .action = act_new_host, .off = off_new_host },
    { .label = "From phone", .nav = true, .action = act_import,
      .arg = SSH_IMPORT_SOFTAP },
    { .label = "From PC",    .nav = true, .action = act_import,
      .arg = SSH_IMPORT_WEB },
};

static int hosts_count(void)
{
    return app.stored_count;
}

static void hosts_row(int i, ui_part_t *out, char *buf, size_t sz)
{
    host_tile(&app.profiles[i], out, buf, sz);
}

static void hosts_open(int i, uint64_t now)
{
    if (i >= 0 && i < app.stored_count) enter_host(now, i);
}

static const menu_list_t hosts_list = {
    .count = hosts_count, .row = hosts_row, .open = hosts_open,
    .off = hosts_shut, .empty = "No hosts yet.", .min_w = 30,
};

static const char *note_hosts(char *buf, size_t sz)
{
    snprintf(buf, sz, "%d stored", app.stored_count);
    return buf;
}

static void act_wifi_kick(intptr_t a, uint64_t now)
{
    (void)a;
    kick_wifi();
    menu_note(now, MENU_MSG_MS, true, "wifi: ...");   /* live-tracks state */
}

static void act_wifiprov(intptr_t a, uint64_t now)
{
    (void)a;
    enter_wifiprov(now);
}

static void act_wifiadd(intptr_t a, uint64_t now)
{
    (void)a;
    enter_wifiadd(now);
}

static const menu_item_t wifi_items[] = {
    { .label = "Reconnect",      .action = act_wifi_kick },
    { .label = "Add from phone", .nav = true, .action = act_wifiprov },
    { .label = "Add by hand",    .nav = true, .action = act_wifiadd },
};

/* The link facts. A down link shows its state word and "-" elsewhere. */
static uint8_t wifi_panel_fill(ui_info_row_t *r, char (*txt)[MENU_PANEL_TXT])
{
    const wifi_mgr_state_t st = wifi_manager_get_state();
    const bool up = st == WIFI_MGR_CONNECTED;
    const char *ip = up ? wifi_manager_get_ip() : "";
    const char *gw = up ? wifi_manager_get_gateway() : "";
    const int rssi = up ? net_rssi() : 0;
    uint8_t pen = 0;
    const char *word = NULL;

    snprintf(txt[0], MENU_PANEL_TXT, "%s", up ? wifi_manager_get_ssid()
                                              : wifi_status_str());
    snprintf(txt[1], MENU_PANEL_TXT, "%s", ip[0] ? ip : "-");
    snprintf(txt[2], MENU_PANEL_TXT, "%s", gw[0] ? gw : "-");
    if (rssi) {
        snprintf(txt[3], MENU_PANEL_TXT, "%d dBm", rssi);
        word = rssi_word(rssi, &pen);
    } else {
        snprintf(txt[3], MENU_PANEL_TXT, "-");
    }
    r[0] = (ui_info_row_t){ .label = "Network", .value = txt[0] };
    r[1] = (ui_info_row_t){ .label = "Address", .value = txt[1] };
    r[2] = (ui_info_row_t){ .label = "Gateway", .value = txt[2] };
    r[3] = (ui_info_row_t){ .label = "Signal", .value = txt[3], .tail = word,
                            .tail_pen = pen };
    return up ? OVERLAY_COL_GREEN
         : st == WIFI_MGR_FAILED ? OVERLAY_COL_RED
         : st == WIFI_MGR_IDLE ? 0 : OVERLAY_COL_AMBER;
}

static const menu_panel_t wifi_panel = { .rows = 4, .fill = wifi_panel_fill };

/* The saved networks, read when the page opens: the list is a file. */
static struct {
    char ssid[STORAGE_WIFI_MAX][33];
    bool locked[STORAGE_WIFI_MAX];
    int  n;
} s_nets;

static void wifi_snap(void)
{
    s_nets.n = app_wifi_list(s_nets.ssid, s_nets.locked, NULL,
                             STORAGE_WIFI_MAX);
}

static int wifi_count(void)
{
    return s_nets.n;
}

static void wifi_row(int i, ui_part_t *out, char *buf, size_t sz)
{
    const bool joined = wifi_manager_get_state() == WIFI_MGR_CONNECTED &&
                        strcmp(wifi_manager_get_ssid(), s_nets.ssid[i]) == 0;
    snprintf(buf, sz, "%d", i + 1);
    *out = (ui_part_t){
        .title = s_nets.ssid[i], .right = buf,
        .detail = joined ? UI_S_CHECK " joined"
                : s_nets.locked[i] ? "locked" : "saved",
        .stripe = joined ? OVERLAY_COL_GREEN : 0,
        .detail_pen = joined ? OVERLAY_COL_GREEN : 0,
    };
}

static void wifi_open(int i, uint64_t now)
{
    if (i >= 0 && i < s_nets.n) enter_wifinet(now, i);
}

static const menu_list_t wifi_list = {
    .count = wifi_count, .row = wifi_row, .open = wifi_open,
    .empty = "No saved networks.", .min_w = 24,
};

static const char *note_wifi(char *buf, size_t sz)
{
    if (wifi_manager_get_state() != WIFI_MGR_CONNECTED) return wifi_status_str();
    snprintf(buf, sz, "%s", wifi_manager_get_ssid());
    return buf;
}

static void act_pair(intptr_t a, uint64_t now)
{
    (void)a;
    enter_pairing(now);
}

static bool off_no_forget(intptr_t a)
{
    (void)a;
    return !app.ble || !app.ble->forget;
}

static void act_forget_bonds(intptr_t a, uint64_t now)
{
    (void)a;
    if (off_no_forget(0)) return;
    app.ble->forget();
    menu_note(now, MENU_MSG_MS, false, "keyboard bonds cleared");
}

static const menu_item_t kbd_items[] = {
    { .label = "Pair keyboard", .nav = true, .action = act_pair },
    { .label = "Forget bonds", .accent = OVERLAY_COL_RED,
      .action = act_forget_bonds, .off = off_no_forget,
      .ask = "Forget every paired keyboard?" },
};

static const char *note_keyboard(char *buf, size_t sz)
{
    if (app.ble && app.ble->get_state && app.ble->get_name &&
        app.ble->get_state() == CYBERDECK_BLE_CONNECTED &&
        app.ble->get_name()[0]) {
        snprintf(buf, sz, "%s connected", app.ble->get_name());
        return buf;
    }
    return ble_status_str();
}

static const char *unavail_ble(void)
{
    return app.ble ? NULL : NOT_BUILT;
}

static bool fx_is_on(intptr_t t)
{
    return app_settings_fx_index((app_fx_tunable_t)t) == 1;
}

static void fx_flip(intptr_t t, uint64_t now)
{
    (void)now;
    app_settings_fx_set((app_fx_tunable_t)t, fx_is_on(t) ? 0 : 1);
}

static int fx_n(intptr_t t)
{
    return app_settings_fx_count((app_fx_tunable_t)t);
}

static int fx_ix(intptr_t t)
{
    return app_settings_fx_index((app_fx_tunable_t)t);
}

static const char *fx_opt(intptr_t t, int i)
{
    return app_settings_fx_label((app_fx_tunable_t)t, i);
}

static void fx_pick(intptr_t t, int i, uint64_t now)
{
    (void)now;
    app_settings_fx_set((app_fx_tunable_t)t, i);
}

/* A tunable with two presets is a switch; more make a choice. */
#define FX_SWITCH(name, t) \
    { .kind = MENU_SWITCH, .label = name, .arg = t, .action = fx_flip, \
      .on = fx_is_on }
#define FX_PICK(name, t) \
    { .kind = MENU_PICK, .label = name, .arg = t, .count = fx_n, \
      .index = fx_ix, .option = fx_opt, .set = fx_pick }

static const menu_item_t fx_crt_items[] = {
    FX_SWITCH("Scanlines", APP_FX_SCAN),
    FX_SWITCH("Bold pop",  APP_FX_BOLD),
    FX_PICK("Phosphor",    APP_FX_MONO),
#if DISPLAY_FX_ROW_GLOW
    FX_PICK("Row glow",    APP_FX_GLOW),
    FX_PICK("Glow decay",  APP_FX_DECAY),
#endif
};

static const menu_item_t fx_motion_items[] = {
    FX_PICK("Wobble",  APP_FX_WOBBLE),
    FX_PICK("Wipe in", APP_FX_WIPE),
    FX_PICK("Collapse", APP_FX_COLLAPSE),
    FX_PICK("Static",  APP_FX_STATIC),
};

/* The tunables that are not at preset 0: "scanlines, phosphor". */
static const char *fx_note(const menu_item_t *items, int n, char *buf,
                           size_t sz)
{
    size_t len = 0;
    buf[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (fx_ix(items[i].arg) == 0) continue;
        if (len && len + 2 < sz) {
            buf[len++] = ',';
            buf[len++] = ' ';
        }
        for (const char *s = items[i].label; *s && len + 1 < sz; s++)
            buf[len++] = (*s >= 'A' && *s <= 'Z') ? (char)(*s + 32) : *s;
        buf[len] = '\0';
    }
    return len ? buf : "all off";
}

static const char *note_fx_crt(char *buf, size_t sz)
{
    return fx_note(fx_crt_items, NELEM(fx_crt_items), buf, sz);
}

static const char *note_fx_motion(char *buf, size_t sz)
{
    return fx_note(fx_motion_items, NELEM(fx_motion_items), buf, sz);
}

/* The i-th size this build has, or FONT_SIZE_COUNT. */
static font_size_t font_nth(int i)
{
    for (int s = 0; s < FONT_SIZE_COUNT; s++)
        if (font_size_available((font_size_t)s) && i-- == 0)
            return (font_size_t)s;
    return FONT_SIZE_COUNT;
}

static int font_n(intptr_t a)
{
    (void)a;
    int n = 0;
    while (font_nth(n) != FONT_SIZE_COUNT) n++;
    return n;
}

static int font_ix(intptr_t a)
{
    (void)a;
    for (int i = 0; font_nth(i) != FONT_SIZE_COUNT; i++)
        if (font_nth(i) == font_active_size()) return i;
    return -1;
}

static const char *font_opt(intptr_t a, int i)
{
    (void)a;
    return font_size_name(font_nth(i));
}

/* Init fixes the grid, the DMA bounce geometry and the DRAM glyph copy, so
 * a size change restarts the deck. The setting goes to disk first. */
static void act_font_apply(intptr_t s, uint64_t now)
{
    const font_size_t want = (font_size_t)s;
    char note[40];

    cyberdeck_font_cfg_t fc;
    snprintf(fc.size, sizeof(fc.size), "%s", font_size_name(want));
    if (storage_kv_save(cyberdeck_settings_ini, cyberdeck_font_section,
                        cyberdeck_font_fields, &fc) != ESP_OK) {
        menu_note(now, MENU_MSG_MS, false, "could not save font");
        return;
    }
#ifndef BUILD_SIMULATOR
    snprintf(note, sizeof(note), "%s set - rebooting", font_size_name(want));
    menu_note(now, MENU_MSG_MS, false, note);
    menu_present_now(now);   /* the blocking delay below means no next tick */
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
#else
    /* The simulator is single-size and cannot restart into another one.
     * It saves the setting anyway, so the device picks it up. */
    snprintf(note, sizeof(note), "%s saved (reboot)", font_size_name(want));
    menu_note(now, MENU_MSG_MS, false, note);
#endif
}

static void font_set(intptr_t a, int i, uint64_t now)
{
    (void)a;
    const font_size_t want = font_nth(i);
    if (want == FONT_SIZE_COUNT || want == font_active_size()) return;
    char title[48];
    snprintf(title, sizeof title, "Restart with the %s font?",
             font_size_name(want));
    ask_open(&(ask_t){
        .title = title,
        .line1 = "The deck restarts to change the font.",
        .line2 = "An open session ends.",
        .verb = "Restart", .accent = OVERLAY_COL_AMBER,
        .yes = act_font_apply, .arg = (intptr_t)want,
    }, now);
}

static int saver_n(intptr_t a)
{
    (void)a;
    return app_settings_saver_count();
}

static int saver_ix(intptr_t a)
{
    (void)a;
    return app_settings_saver_index();
}

static const char *saver_opt(intptr_t a, int i)
{
    (void)a;
    return app_settings_saver_label(i);
}

static void saver_set(intptr_t a, int i, uint64_t now)
{
    (void)a; (void)now;
    app_settings_saver_set(i);
}

#if CONFIG_INPUT_TOUCH_SCROLL
static bool touch_on(intptr_t a)
{
    (void)a;
    return app.touch_scroll;
}

static void act_touch(intptr_t a, uint64_t now)
{
    (void)a; (void)now;
    app_settings_touch_toggle();
}
#endif

static void act_clearhosts(intptr_t a, uint64_t now)
{
    (void)a;
    esp_err_t e = storage_known_hosts_clear();
    menu_note(now, MENU_MSG_MS, false,
              e == ESP_OK ? "host keys cleared" : "nothing to clear");
}

static void act_factory(intptr_t a, uint64_t now)
{
    (void)a;
    storage_factory_reset();
    app_settings_dirty_discard();   /* pending writes must not resurrect it */
    if (app.ble && app.ble->forget) app.ble->forget();
    load_profiles();
    menu_note(now, MENU_MSG_MS, false, "wiped - reboot advised");
}

/* Danger items sit at the bottom of the page, apart from the rest. */
static const menu_item_t system_items[] = {
    { .kind = MENU_PICK, .label = "Font size", .count = font_n,
      .index = font_ix, .option = font_opt, .set = font_set },
    { .kind = MENU_PICK, .label = "Saver + lock, min", .count = saver_n,
      .index = saver_ix, .option = saver_opt, .set = saver_set },
#if CONFIG_INPUT_TOUCH_SCROLL
    { .kind = MENU_SWITCH, .label = "Edge scroll", .action = act_touch,
      .on = touch_on },
#endif
    { .label = "Clear host keys", .accent = OVERLAY_COL_RED, .danger = true,
      .action = act_clearhosts, .ask = "Clear every pinned host key?" },
    { .label = "Factory reset", .accent = OVERLAY_COL_RED, .danger = true,
      .action = act_factory, .ask = "Erase every host and setting?" },
};

static const char *note_system(char *buf, size_t sz)
{
    if (!app.cfg.version || !app.cfg.version[0]) return "saver, reset";
    snprintf(buf, sz, "version %s", app.cfg.version);
    return buf;
}

/* The page reads the store state once, when it opens. An absent store
 * would stat the filesystem ten times a second. */
static keystore_state_t s_ks_snap;

static void ks_snap(void)
{
    s_ks_snap = keystore_state();
}

static bool hidden_ks_absent(intptr_t a)
{
    (void)a;
    return s_ks_snap == KEYSTORE_ABSENT;
}

static const char *label_ks_setpin(intptr_t a)
{
    (void)a;
    return s_ks_snap == KEYSTORE_ABSENT ? "Create keystore" : "Change code";
}

static void act_ks_lock(intptr_t a, uint64_t now)
{
    (void)a;                          /* panic button: park the deck */
    keystore_lock();
    app_creds_wipe();
    unlock_open_gate(now);
}

static void act_ks_setpin(intptr_t a, uint64_t now)
{
    (void)a;
    unlock_open_setpin(now);          /* create (store absent) or change */
}

static void act_ks_remove(intptr_t a, uint64_t now)
{
    (void)a;
    unlock_open_remove(now);          /* the code pad asks and proves */
}

static const menu_item_t ks_items[] = {
    { .label = "Lock deck", .action = act_ks_lock,
      .hidden = hidden_ks_absent },
    { .label_fn = label_ks_setpin, .nav = true, .action = act_ks_setpin },
    { .label = "Remove code", .nav = true, .accent = OVERLAY_COL_RED,
      .danger = true, .action = act_ks_remove, .hidden = hidden_ks_absent },
};

static const char *note_keystore(char *buf, size_t sz)
{
    (void)buf; (void)sz;
    return s_ks_snap == KEYSTORE_UNLOCKED ? "unlocked"
         : s_ks_snap == KEYSTORE_ABSENT   ? "no access code" : "locked";
}

static const char *unavail_keystore(void)
{
#if CONFIG_CYBERDECK_KEYSTORE
    return NULL;
#else
    return NOT_BUILT;
#endif
}

#define ASSERT_FITS(t) \
    _Static_assert(NELEM(t) <= MENU_ITEM_MAX, "section has too many items")
ASSERT_FITS(hosts_items);     ASSERT_FITS(wifi_items);
ASSERT_FITS(kbd_items);       ASSERT_FITS(fx_crt_items);
ASSERT_FITS(fx_motion_items); ASSERT_FITS(system_items);
ASSERT_FITS(ks_items);

static const menu_page_t PAGES[MS_COUNT] = {
    [MS_MAIN]      = { .title = "Session", .back_to = MENU_BACK_LEAVE },
    [MS_CONFIG]    = { .title = "Setup", .back_to = MS_MAIN,
                       .on_open = ks_snap },
    [MS_HOSTS]     = { .title = "Hosts", .items = hosts_items,
                       .list = &hosts_list,
                       .count = NELEM(hosts_items), .back_to = MS_CONFIG,
                       .note = note_hosts },
    [MS_WIFI]      = { .title = "Wi-Fi", .items = wifi_items,
                       .panel = &wifi_panel, .list = &wifi_list,
                       .count = NELEM(wifi_items), .back_to = MS_CONFIG,
                       .on_open = wifi_snap, .note = note_wifi },
    [MS_KEYBOARD]  = { .title = "Keyboard", .items = kbd_items,
                       .count = NELEM(kbd_items), .back_to = MS_CONFIG,
                       .note = note_keyboard, .unavail = unavail_ble },
    [MS_FX_CRT]    = { .title = "CRT FX", .items = fx_crt_items,
                       .count = NELEM(fx_crt_items), .back_to = MS_CONFIG,
                       .hold = APP_SETTINGS_HOLD_FX, .note = note_fx_crt },
    [MS_FX_MOTION] = { .title = "Motion FX", .items = fx_motion_items,
                       .count = NELEM(fx_motion_items), .back_to = MS_CONFIG,
                       .hold = APP_SETTINGS_HOLD_FX, .note = note_fx_motion },
    [MS_SYSTEM]    = { .title = "System", .items = system_items,
                       .count = NELEM(system_items), .back_to = MS_CONFIG,
                       .hold = APP_SETTINGS_HOLD_SYS, .note = note_system },
    [MS_KEYSTORE]  = { .title = "Keystore", .items = ks_items,
                       .count = NELEM(ks_items), .back_to = MS_CONFIG,
                       .on_open = ks_snap, .note = note_keystore,
                       .unavail = unavail_keystore },
};

static const uint8_t HUB[] = {
    MS_HOSTS, MS_WIFI, MS_KEYBOARD, MS_FX_CRT, MS_FX_MOTION,
    MS_SYSTEM, MS_KEYSTORE,
};

const menu_page_t *menu_page(int sc)
{
    if (sc < 0 || sc >= MS_COUNT) return NULL;
    return &PAGES[sc];
}

const uint8_t *menu_hub(int *n)
{
    *n = NELEM(HUB);
    return HUB;
}
