/*
 * app_menu_defs.c — the menu tree as data: page tables whose items carry
 * their own action/confirm/dim/value behavior. See app_menu_defs.h;
 * rendering and the dynamic pickers live in app_menu.c.
 */

#include "app_menu_defs.h"
#include "app_internal.h"
#include "app_screens.h"
#include "app_settings.h"
#include "font.h"
#include "keystore.h"
#include "ssh_client.h"

#ifdef ESP_PLATFORM
#include "sdkconfig.h"       /* CONFIG_CYBERDECK_KEYSTORE (sim: -D flag) */
#endif

#include <stdio.h>
#include <string.h>

#ifndef BUILD_SIMULATOR
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"     /* esp_restart() — applying a new font size */
#endif

/* Menu color law — one color per KIND of item, not per item (a page of
 * many-colored bars reads as motley):
 *   CYAN  = normal action / navigation      GREEN = go / primary
 *   RED   = destructive                     AMBER = caution
 *   BLUE  = Back / Cancel (safe exit)
 * MAGENTA stays reserved for the title lozenge. */

/* ------------------------------------------------------- shared actions */

static void act_back(intptr_t a, uint64_t now)
{
    (void)a;
    menu_back(now);
}

static void act_goto(intptr_t sc, uint64_t now)
{
    (void)now;
    menu_goto((int)sc);
}

/* ----------------------------------------------------------------- MAIN */

static void act_disconnect(intptr_t a, uint64_t now)
{
    (void)a;
    ssh_client_disconnect();
    enter_home_after_collapse(now);   /* deliberate CRT power-off */
}

static const menu_item_t main_items[] = {
    { .label = "Resume session",  .color = OVERLAY_COL_GREEN, .action = act_back },
    { .label = "Disconnect",      .color = OVERLAY_COL_AMBER, .action = act_disconnect },
    { .label = "Configuration >", .color = OVERLAY_COL_CYAN,  .action = act_goto,
      .arg = MS_CONFIG },
};

/* --------------------------------------------------------------- CONFIG */

static bool dim_no_ble(intptr_t a)
{
    (void)a;
    return !app.cfg.ble;
}

static bool dim_no_keystore(intptr_t a)
{
    (void)a;
#if CONFIG_CYBERDECK_KEYSTORE
    return false;
#else
    return true;   /* entry stays (stable page), visibly unavailable */
#endif
}

static const menu_item_t config_items[] = {
    { .label = "Profiles >", .color = OVERLAY_COL_CYAN, .action = act_goto,
      .arg = MS_PROFILES },
    { .label = "WiFi >",     .color = OVERLAY_COL_CYAN, .action = act_goto,
      .arg = MS_WIFI },
    { .label = "Keyboard >", .color = OVERLAY_COL_CYAN, .action = act_goto,
      .arg = MS_KEYBOARD, .dim = dim_no_ble, .dim_note = "no BLE keyboard support" },
    { .label = "Effects >",  .color = OVERLAY_COL_CYAN, .action = act_goto,
      .arg = MS_EFFECTS },
    { .label = "Font >",     .color = OVERLAY_COL_CYAN, .action = act_goto,
      .arg = MS_FONT },
    { .label = "Keystore >", .color = OVERLAY_COL_CYAN, .action = act_goto,
      .arg = MS_KEYSTORE, .dim = dim_no_keystore, .dim_note = "not in this build" },
    { .label = "System >",   .color = OVERLAY_COL_CYAN, .action = act_goto,
      .arg = MS_SYSTEM },
    { .label = "Back",       .color = OVERLAY_COL_BLUE, .action = act_back },
};

/* ------------------------------------------------------------- PROFILES */

static void act_prof_add(intptr_t a, uint64_t now)
{
    (void)a;
    if (app.stored_count >= MAX_PROFILES - 1) {
        menu_note(now, MENU_MSG_MS, false, "profile list full");
        return;
    }
    enter_profile(now, -1);
}

static void act_prof_edit(intptr_t a, uint64_t now)
{
    (void)a;
    if (app.stored_count == 0) {
        menu_note(now, MENU_MSG_MS, false, "no stored profiles");
        return;
    }
    menu_goto(MS_EDITPROFILE);
}

static void act_prof_reorder(intptr_t a, uint64_t now)
{
    (void)a;
    if (app.stored_count < 2) {
        menu_note(now, MENU_MSG_MS, false, "nothing to reorder");
        return;
    }
    menu_goto(MS_REORDER);   /* entering the picker resets the grab */
}

static const menu_item_t profiles_items[] = {
    { .label = "Add (type here)", .color = OVERLAY_COL_CYAN, .action = act_prof_add },
    { .label = "Edit",            .color = OVERLAY_COL_CYAN, .action = act_prof_edit },
    { .label = "Reorder",         .color = OVERLAY_COL_CYAN, .action = act_prof_reorder },
    { .label = "Delete",          .color = OVERLAY_COL_RED,  .action = act_goto,
      .arg = MS_DELPROFILE },
    { .label = "Import >",        .color = OVERLAY_COL_CYAN, .action = act_goto,
      .arg = MS_IMPORT },
    { .label = "Back",            .color = OVERLAY_COL_BLUE, .action = act_back },
};

/* --------------------------------------------------------------- IMPORT */

static void act_import(intptr_t mode, uint64_t now)
{
    enter_sshimport(now, (ssh_import_mode_t)mode);
}

static const menu_item_t import_items[] = {
    { .label = "SoftAP (phone)", .color = OVERLAY_COL_CYAN, .action = act_import,
      .arg = SSH_IMPORT_SOFTAP },
    { .label = "Web (PC)",       .color = OVERLAY_COL_CYAN, .action = act_import,
      .arg = SSH_IMPORT_WEB },
    { .label = "Back",           .color = OVERLAY_COL_BLUE, .action = act_back },
};

/* ----------------------------------------------------------------- WIFI */

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

static const menu_item_t wifi_items[] = {
    { .label = "Reconnect",           .color = OVERLAY_COL_CYAN, .action = act_wifi_kick },
    { .label = "Add network (phone)", .color = OVERLAY_COL_CYAN, .action = act_wifiprov },
    { .label = "Back",                .color = OVERLAY_COL_BLUE, .action = act_back },
};

/* ------------------------------------------------------------- KEYBOARD */

static void act_pair(intptr_t a, uint64_t now)
{
    (void)a;
    enter_pairing(now);
}

static bool dim_no_forget(intptr_t a)
{
    (void)a;
    return !app.cfg.ble || !app.cfg.ble->forget;
}

static void act_forget_bonds(intptr_t a, uint64_t now)
{
    (void)a;
    app.cfg.ble->forget();   /* dim gate guarantees the op exists */
    menu_note(now, MENU_MSG_MS, false, "keyboard bonds cleared");
}

static const menu_item_t kbd_items[] = {
    { .label = "Pair keyboard", .color = OVERLAY_COL_CYAN, .action = act_pair,
      .dim = dim_no_ble, .dim_note = "no BLE keyboard support" },
    { .label = "Forget bonds",  .color = OVERLAY_COL_RED,  .action = act_forget_bonds,
      .confirm = "CONFIRM forget bonds?", .arm_note = "activate again to forget",
      .dim = dim_no_forget, .dim_note = "forget unavailable" },
    { .label = "Back",          .color = OVERLAY_COL_BLUE, .action = act_back },
};

/* --------------------------------------------------------------- SYSTEM */

static void act_saver(intptr_t a, uint64_t now)
{
    (void)a; (void)now;
    app_settings_saver_cycle();
}

static const char *val_saver(intptr_t a, char *buf, size_t sz)
{
    (void)a;
    app_settings_saver_format(buf, sz);
    return buf;
}

#if CONFIG_INPUT_TOUCH_SCROLL
static void act_touch(intptr_t a, uint64_t now)
{
    (void)a; (void)now;
    app_settings_touch_toggle();
}

static const char *val_touch(intptr_t a, char *buf, size_t sz)
{
    (void)a; (void)buf; (void)sz;
    return app_settings_touch_str();
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
    if (app.cfg.ble && app.cfg.ble->forget) app.cfg.ble->forget();
    load_profiles();
    menu_note(now, MENU_MSG_MS, false, "wiped - reboot advised");
}

static const menu_item_t system_items[] = {
    { .label = "Saver + lock after",  .color = OVERLAY_COL_CYAN,
      .action = act_saver, .value = val_saver },
#if CONFIG_INPUT_TOUCH_SCROLL
    { .label = "Edge scroll gesture", .color = OVERLAY_COL_CYAN,
      .action = act_touch, .value = val_touch },
#endif
    { .label = "Clear host keys",     .color = OVERLAY_COL_RED,
      .action = act_clearhosts, .confirm = "CONFIRM clear host keys?",
      .arm_note = "activate again to clear" },
    { .label = "Factory reset",       .color = OVERLAY_COL_RED,
      .action = act_factory, .confirm = "CONFIRM FACTORY RESET?",
      .arm_note = "activate again to WIPE ALL" },
    { .label = "Back",                .color = OVERLAY_COL_BLUE, .action = act_back },
};

/* -------------------------------------------------------------- EFFECTS
 * Every runtime render-fx tunable as a value-cycling tile. Cycling +
 * persistence live in the settings model (app_settings.c). */

static void act_fx(intptr_t t, uint64_t now)
{
    (void)now;
    app_settings_fx_cycle((app_fx_tunable_t)t);
}

static const char *val_fx(intptr_t t, char *buf, size_t sz)
{
    (void)buf; (void)sz;
    const app_fx_tunable_t fx = (app_fx_tunable_t)t;
    return app_settings_fx_label(fx, app_settings_fx_index(fx));
}

static const menu_item_t fx_items[] = {
    { .label = "Scanlines",  .color = OVERLAY_COL_CYAN, .action = act_fx,
      .arg = APP_FX_SCAN,     .value = val_fx },
    { .label = "Phosphor",   .color = OVERLAY_COL_CYAN, .action = act_fx,
      .arg = APP_FX_MONO,     .value = val_fx },
    { .label = "Bold pop",   .color = OVERLAY_COL_CYAN, .action = act_fx,
      .arg = APP_FX_BOLD,     .value = val_fx },
    { .label = "Wobble",     .color = OVERLAY_COL_CYAN, .action = act_fx,
      .arg = APP_FX_WOBBLE,   .value = val_fx },
#if DISPLAY_FX_ROW_GLOW
    { .label = "Row glow",   .color = OVERLAY_COL_CYAN, .action = act_fx,
      .arg = APP_FX_GLOW,     .value = val_fx },
    { .label = "Glow decay", .color = OVERLAY_COL_CYAN, .action = act_fx,
      .arg = APP_FX_DECAY,    .value = val_fx },
#endif
    { .label = "Wipe in",    .color = OVERLAY_COL_CYAN, .action = act_fx,
      .arg = APP_FX_WIPE,     .value = val_fx },
    { .label = "Collapse",   .color = OVERLAY_COL_CYAN, .action = act_fx,
      .arg = APP_FX_COLLAPSE, .value = val_fx },
    { .label = "Static",     .color = OVERLAY_COL_CYAN, .action = act_fx,
      .arg = APP_FX_STATIC,   .value = val_fx },
    { .label = "Back",       .color = OVERLAY_COL_BLUE, .action = act_back },
};

/* ----------------------------------------------------------------- FONT */

/* What the next boot will use — the active size unless a tile tapped this
 * session already wrote [font]. Resolved when the page OPENS, not per
 * render: the menu renders ~10 Hz and settings.ini lives on littlefs. */
static font_size_t s_font_pending = FONT_SIZE_COUNT;   /* COUNT = unresolved */

static void font_on_open(void)
{
    s_font_pending = FONT_SIZE_COUNT;
}

static void font_refresh_pending(void)
{
    s_font_pending = font_active_size();
    cyberdeck_font_cfg_t fc = { .size = "" };
    if (storage_kv_load(cyberdeck_settings_ini, cyberdeck_font_section,
                        cyberdeck_font_fields, &fc) == ESP_OK &&
        fc.size[0]) {
        for (int i = 0; i < FONT_SIZE_COUNT; i++)
            if (strcmp(fc.size, font_size_name((font_size_t)i)) == 0) {
                s_font_pending = (font_size_t)i;
                break;
            }
    }
}

static const char *label_font(intptr_t s)
{
    return font_size_name((font_size_t)s);
}

static uint8_t color_font(intptr_t s)
{
    return (font_size_t)s == font_active_size() ? OVERLAY_COL_GREEN
                                                : OVERLAY_COL_CYAN;
}

static const char *val_font(intptr_t s, char *buf, size_t sz)
{
    (void)buf; (void)sz;
    if (s_font_pending >= FONT_SIZE_COUNT) font_refresh_pending();
    const font_size_t fs = (font_size_t)s;
    return !font_size_available(fs) ? "not in this build"
         : fs == font_active_size() ? "in use"
         : fs == s_font_pending     ? "reboot to apply"
                                    : "";
}

static void act_font(intptr_t s, uint64_t now)
{
    const font_size_t want = (font_size_t)s;
    char note[40];

    if (!font_size_available(want)) {
        snprintf(note, sizeof(note), "%s not in this build",
                 font_size_name(want));
        menu_note(now, MENU_MSG_MS, false, note);
        return;
    }
    if (want == font_active_size()) {
        snprintf(note, sizeof(note), "%s already in use",
                 font_size_name(want));
        menu_note(now, MENU_MSG_MS, false, note);
        return;
    }
    cyberdeck_font_cfg_t fc;
    snprintf(fc.size, sizeof(fc.size), "%s", font_size_name(want));
    if (storage_kv_save(cyberdeck_settings_ini, cyberdeck_font_section,
                        cyberdeck_font_fields, &fc) != ESP_OK) {
        menu_note(now, MENU_MSG_MS, false, "could not save font");
        return;
    }
    /* The grid, DMA bounce geometry and DRAM glyph copy are fixed at
     * init, so a size change needs a restart. The setting is already
     * on disk — paint the note, hold it readable, reboot. */
#ifndef BUILD_SIMULATOR
    snprintf(note, sizeof(note), "%s set - rebooting", font_size_name(want));
    menu_note(now, MENU_MSG_MS, false, note);
    menu_present_now(now);   /* the blocking delay below means no next tick */
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
#else
    /* The simulator is single-size and cannot restart into another one;
     * the setting is saved so the device picks it up. */
    snprintf(note, sizeof(note), "%s saved (reboot)", font_size_name(want));
    menu_note(now, MENU_MSG_MS, false, note);
#endif
}

static const menu_item_t font_items[] = {
    { .label_fn = label_font, .color_fn = color_font, .action = act_font,
      .arg = FONT_SIZE_8X16,  .value = val_font },
    { .label_fn = label_font, .color_fn = color_font, .action = act_font,
      .arg = FONT_SIZE_10X20, .value = val_font },
    { .label_fn = label_font, .color_fn = color_font, .action = act_font,
      .arg = FONT_SIZE_12X24, .value = val_font },
    { .label = "Back", .color = OVERLAY_COL_BLUE, .action = act_back },
};

/* ------------------------------------------------------------- KEYSTORE
 * The page is CONTEXTUAL — impossible actions are hidden, not no-op'd
 * (no "Lock deck" or "Remove code" without a store). Two-gates model: a
 * store on the deck means the deck locks at boot/wake. */

/* Store state resolved when the page OPENS, not per render: the menu
 * renders ~10 Hz and an ABSENT store would stat the filesystem per frame. */
static keystore_state_t s_ks_snap;

static void ks_on_open(void)
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

static const char *val_ks_lock(intptr_t a, char *buf, size_t sz)
{
    (void)a; (void)buf; (void)sz;
    return s_ks_snap == KEYSTORE_UNLOCKED ? "unlocked" : "locked";
}

static const char *val_ks_setpin(intptr_t a, char *buf, size_t sz)
{
    (void)a; (void)buf; (void)sz;
    return s_ks_snap == KEYSTORE_ABSENT ? "locks the deck" : "";
}

static const char *val_ks_remove(intptr_t a, char *buf, size_t sz)
{
    (void)a; (void)buf; (void)sz;
    return "keys to plain";
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
    unlock_open_remove(now);          /* proves the code first */
}

static const menu_item_t ks_items[] = {
    { .label = "Lock deck",   .color = OVERLAY_COL_CYAN, .action = act_ks_lock,
      .hidden = hidden_ks_absent, .value = val_ks_lock },
    { .label_fn = label_ks_setpin, .color = OVERLAY_COL_CYAN,
      .action = act_ks_setpin, .value = val_ks_setpin },
    { .label = "Remove code", .color = OVERLAY_COL_CYAN, .action = act_ks_remove,
      .hidden = hidden_ks_absent, .value = val_ks_remove },
    { .label = "Back",        .color = OVERLAY_COL_BLUE, .action = act_back },
};

/* ------------------------------------------------------------ the pages */

#define ASSERT_FITS(t) \
    _Static_assert(NELEM(t) <= MENU_MAX_TILES, "grow MENU_MAX_TILES")
ASSERT_FITS(main_items);   ASSERT_FITS(config_items);
ASSERT_FITS(profiles_items); ASSERT_FITS(import_items);
ASSERT_FITS(wifi_items);   ASSERT_FITS(kbd_items);
ASSERT_FITS(system_items); ASSERT_FITS(fx_items);
ASSERT_FITS(font_items);   ASSERT_FITS(ks_items);

static const menu_page_t PAGES[] = {
    [MS_MAIN]     = { .title = "MENU", .items = main_items,
                      .count = NELEM(main_items), .back_to = MENU_BACK_LEAVE },
    [MS_CONFIG]   = { .title = "CONFIGURATION", .items = config_items,
                      .count = NELEM(config_items), .back_to = MS_MAIN },
    [MS_PROFILES] = { .title = "PROFILES", .items = profiles_items,
                      .count = NELEM(profiles_items), .back_to = MS_CONFIG },
    [MS_IMPORT]   = { .title = "IMPORT", .items = import_items,
                      .count = NELEM(import_items), .back_to = MS_PROFILES },
    [MS_WIFI]     = { .title = "WIFI", .items = wifi_items,
                      .count = NELEM(wifi_items), .back_to = MS_CONFIG },
    [MS_KEYBOARD] = { .title = "KEYBOARD", .items = kbd_items,
                      .count = NELEM(kbd_items), .back_to = MS_CONFIG },
    [MS_SYSTEM]   = { .title = "SYSTEM", .items = system_items,
                      .count = NELEM(system_items), .back_to = MS_CONFIG,
                      .hold = APP_SETTINGS_HOLD_SYS },
    [MS_EFFECTS]  = { .title = "EFFECTS", .items = fx_items,
                      .count = NELEM(fx_items), .back_to = MS_CONFIG,
                      .flags = MENU_PAGE_WIDE | MENU_PAGE_VALS,
                      .hold = APP_SETTINGS_HOLD_FX },
    [MS_FONT]     = { .title = "FONT", .items = font_items,
                      .count = NELEM(font_items), .back_to = MS_CONFIG,
                      .on_open = font_on_open },
    [MS_KEYSTORE] = { .title = "KEYSTORE", .items = ks_items,
                      .count = NELEM(ks_items), .back_to = MS_CONFIG,
                      .on_open = ks_on_open },
    /* MS_DELPROFILE / MS_EDITPROFILE / MS_REORDER: dynamic pickers. */
};

const menu_page_t *menu_page(int sc)
{
    if (sc < 0 || sc >= (int)NELEM(PAGES) || !PAGES[sc].items) return NULL;
    return &PAGES[sc];
}

bool menu_is_picker(int sc)
{
    return sc == MS_DELPROFILE || sc == MS_EDITPROFILE || sc == MS_REORDER;
}
