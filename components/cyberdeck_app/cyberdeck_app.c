/*
 * cyberdeck_app.c — the shell's core: shared state, init, dispatch.
 *
 * BOOT → HOME → CONNECTING → SESSION, with PAIRING/HOSTKEY/MENU/PROFILE/
 * WIFIPROV/SSHIMPORT as modals. All shell UI lives in the display overlay
 * layer (app_ui); the vterm cell buffer belongs to the boot splash and the
 * SSH session. This file owns the shared state instance, decodes input
 * once, and dispatches tick/input to the module for app.state.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_settings.h"
#include "cyberdeck_plugin.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "display_fx.h"
#include "keystore.h"       /* keystore_wipe for the cred scratch */
#include "storage_cred.h"   /* the shared credential staging buffer */
#include "vtkeys.h"         /* HIDKEY events -> ui_key_t */
#include "wifi_manager.h"

static const char *TAG = "cyberdeck_app";

/* The one shared shell state instance (declared in app_internal.h). */
struct app_state app;

static ui_key_t decode_key(const cyberdeck_input_t *ev, char *ch)
{
    if (ev->type == CYBERDECK_INPUT_HIDKEY) {
        const bool shift = vtkeys_mods_from_hid(ev->mods) & VTMOD_SHIFT;
        switch (vtkeys_from_hid(ev->key)) {
        case VTKEY_UP:    return K_UP;
        case VTKEY_DOWN:  return K_DOWN;
        case VTKEY_LEFT:  return K_LEFT;
        case VTKEY_RIGHT: return K_RIGHT;
        case VTKEY_F12:   return K_F12;
        /* Shift+PageUp / Shift+PageDown — scrollback, kept by the deck. */
        case VTKEY_PGUP:  return shift ? K_SCROLL_UP   : K_NONE;
        case VTKEY_PGDN:  return shift ? K_SCROLL_DOWN : K_NONE;
        default:          return K_NONE;
        }
    }

    const uint8_t *b = ev->buf;
    if (ev->len == 1) {
        if (b[0] == 0x1B) return K_ESC;
        if (b[0] == '\r' || b[0] == '\n') return K_ENTER;
        if (b[0] == 0x08 || b[0] == 0x7F) return K_BACKSPACE;
        if (b[0] == 0x09) return K_TAB;
        if (b[0] >= 0x20 && b[0] < 0x7F) { if (ch) *ch = (char)b[0]; return K_CHAR; }
    }
    return K_NONE;
}

/* The strip width is a build-time constant; only the on/off state is a
 * runtime setting, so "apply" is simply width-or-zero. This call goes
 * through the platform seam, so the driver alone owns the armed
 * state. A stale copy of app state can therefore never leave it
 * armed. */
void app_touch_scroll_apply(void)
{
    if (!app.cfg.set_scroll_edge) return;
    app.cfg.set_scroll_edge(app.touch_scroll ? app.cfg.scroll_edge_px : 0);
}

void load_profiles(void)
{
    app.profile_count = 0;
    int n = 0;
    if (storage_load_profiles(app.profiles, &n, MAX_PROFILES - 1) != ESP_OK)
        n = 0;
    app.profile_count = n;
    app.stored_count  = n;   /* real, on-flash profiles (before any synth below) */

    /* Synthesize "(default)" from the Kconfig fallback ONLY when profiles.ini
     * gave us nothing — a populated file must not get a redundant extra. */
    if (n == 0 && app.cfg.fallback_host && app.cfg.fallback_host[0]) {
        conn_profile_t *p = &app.profiles[app.profile_count++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "(default)");
        snprintf(p->host, sizeof(p->host), "%s", app.cfg.fallback_host);
        p->port = app.cfg.fallback_port ? app.cfg.fallback_port : 22;
        snprintf(p->user, sizeof(p->user), "%s",
                 app.cfg.fallback_user ? app.cfg.fallback_user : "root");
        p->auth = STORAGE_AUTH_PASSWORD;
        snprintf(p->password, sizeof(p->password), "%s",
                 app.cfg.fallback_password ? app.cfg.fallback_password : "");
    }
}

/* Lock companion — see app_internal.h. keystore_lock() wipes the MK
 * and the secrets cache inside the vault. But load_profiles() has
 * already copied the hydrated passwords out here. Three places carry
 * these plaintext secrets: HOME's list (app.profiles), the connect
 * snapshot (conn), and the editor draft (profile). Each one holds a
 * login password or a key passphrase. Without this wipe, the panic
 * button and the idle auto-lock would leave every credential sitting
 * in .bss. The lock would then only really cover the key files.
 *
 * Only the secret fields go. Names, hosts, users, and ports are
 * explicitly not secret (docs/storage_auth.md "orthogonal model" axis
 * 1). HOME also draws its tile grid before any unlock. The next
 * successful unlock re-hydrates via load_profiles(). */
void app_creds_wipe(void)
{
    for (int i = 0; i < MAX_PROFILES; i++)
        keystore_wipe(app.profiles[i].password,
                      sizeof(app.profiles[i].password));
    conn_creds_wipe();      /* the active-connect snapshot */
    profile_creds_wipe();   /* the editor draft            */
}

const void *cyberdeck_service(const char *name)
{
    for (int i = 0; i < app.cfg.n_services; i++)
        if (strcmp(app.cfg.services[i].name, name) == 0)
            return app.cfg.services[i].ops;
    return NULL;
}

bool ble_has_bond(void)
{
    if (!app.ble) return false;
    ble_device_info_t d[STORAGE_BLE_MAX];
    int n = 0;
    storage_ble_list(d, STORAGE_BLE_MAX, &n);
    return n > 0;
}

/* This is a one-time migration. A past firmware persisted a WiFi
 * credential into the driver's NVS. wifi init captures it, then folds
 * it into storage (the secrets bundle) unless the SSID is already
 * known. The store is open on the post-unlock path that calls this. */
void wifi_migrate_nvs_cred(void)
{
    /* Shared cred scratch (storage.h) — never on this stack: the main
     * task has 3.5 KB and this runs on the kick_wifi call chain. */
    storage_cred_scratch_t *sc = storage_cred_scratch();
    if (!wifi_manager_take_nvs_cred(&sc->one)) return;

    wifi_profile_t *nets = sc->u.nets;
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);
    bool known = false;
    for (int i = 0; i < n && !known; i++)
        known = strcmp(nets[i].ssid, sc->one.ssid) == 0;
    if (!known && n < STORAGE_WIFI_MAX) {
        nets[n++] = sc->one;
        if (storage_wifi_save(nets, n) == ESP_OK) {
            ESP_LOGW(TAG, "Migrated WiFi credential '%s' from driver NVS "
                          "into storage", sc->one.ssid);
            /* Saved and verifiable — ONLY NOW may the NVS copy go. */
            wifi_manager_clear_nvs_cred();
        }
    } else if (known) {
        wifi_manager_clear_nvs_cred();   /* already in storage: redundant */
    }
    keystore_wipe(sc, sizeof(*sc));
}

/* One-time seed: a Kconfig fallback credential with a real PSK moves
 * into storage, so a later rebuild can blank sdkconfig. A firmware
 * image must not carry a usable pre-shared key in .rodata. Idempotent:
 * a known SSID (plaintext or @bundle marker row) is never re-seeded. */
static void wifi_seed_fallback(void)
{
    if (!app.cfg.fallback_wifi_ssid || !app.cfg.fallback_wifi_ssid[0] ||
        !app.cfg.fallback_wifi_password || !app.cfg.fallback_wifi_password[0])
        return;

    wifi_profile_t *nets = storage_cred_scratch()->u.nets;
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);
    for (int i = 0; i < n; i++)
        if (strcmp(nets[i].ssid, app.cfg.fallback_wifi_ssid) == 0) {
            keystore_wipe(nets, sizeof(*nets) * STORAGE_WIFI_MAX);
            return;
        }
    if (n < STORAGE_WIFI_MAX) {
        memset(&nets[n], 0, sizeof(nets[n]));
        snprintf(nets[n].ssid, sizeof(nets[n].ssid), "%s",
                 app.cfg.fallback_wifi_ssid);
        snprintf(nets[n].password, sizeof(nets[n].password), "%s",
                 app.cfg.fallback_wifi_password);
        n++;
        if (storage_wifi_save(nets, n) == ESP_OK)
            ESP_LOGW(TAG, "Seeded fallback WiFi '%s' into storage — blank "
                          "CONFIG_WIFI_PASSWORD now",
                     app.cfg.fallback_wifi_ssid);
    }
    keystore_wipe(nets, sizeof(*nets) * STORAGE_WIFI_MAX);
}

void kick_wifi(void)
{
    /* Init is idempotent and captures any credential a past firmware left
     * in the driver's NVS; the migration folds it into storage — the
     * bundle when the store is open, plaintext wifi.ini otherwise (a
     * store-less deck keeps working; a locked one adopts at unlock). */
    wifi_manager_init();
    wifi_migrate_nvs_cred();
    wifi_seed_fallback();

    /* Both helpers above have finished with the shared scratch. */
    wifi_profile_t *nets = storage_cred_scratch()->u.nets;
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);

    /* Rows still wearing the @bundle marker are unreadable until unlock —
     * joining with the literal marker (or as an open net) would only
     * thrash the AP. They rejoin at the post-unlock kick. */
    int usable = 0, gated = 0;
    for (int i = 0; i < n; i++) {
        if (strcmp(nets[i].password, STORAGE_PW_BUNDLED) == 0) {
            gated++;
            continue;
        }
        if (usable != i) nets[usable] = nets[i];
        usable++;
    }
    if (gated) ESP_LOGI(TAG, "%d WiFi network(s) await unlock", gated);
    n = usable;

    if (n == 0 && app.cfg.fallback_wifi_ssid && app.cfg.fallback_wifi_ssid[0]) {
        memset(&nets[0], 0, sizeof(nets[0]));
        snprintf(nets[0].ssid, sizeof(nets[0].ssid), "%s",
                 app.cfg.fallback_wifi_ssid);
        snprintf(nets[0].password, sizeof(nets[0].password), "%s",
                 app.cfg.fallback_wifi_password ? app.cfg.fallback_wifi_password : "");
        n = 1;
    }

    if (n > 0) {
        wifi_manager_connect(nets, n);     /* copies what it needs */
    } else if (gated == 0) {               /* gated nets already logged */
        ESP_LOGW(TAG, "no WiFi profiles (wifi.ini empty, no fallback)");
    }
    keystore_wipe(nets, sizeof(*nets) * STORAGE_WIFI_MAX);
}

void toast_for(uint64_t now, uint32_t ms, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app.toast, sizeof(app.toast), fmt, ap);
    va_end(ap);
    app.toast_until = now + ms;
    app.toast_ok    = false;   /* only enter_session() garnishes with a checkmark */
}

/* Watch the wifi + BLE keyboard links and toast every transition the user
 * cares about. Pairing-scan churn is deliberately silent — the pairing
 * screen narrates itself, and leaving it for a scan is not a "disconnect". */
static void status_toasts(uint64_t now)
{
    uint8_t w = (uint8_t)wifi_manager_get_state();
    if (w != app.prev_wifi) {
        if (w == WIFI_MGR_CONNECTED)
            toast(now, "wifi connected %s", wifi_manager_get_ip());
        else if (w == WIFI_MGR_CONNECTING)
            toast(now, "wifi connecting...");
        else if (w == WIFI_MGR_FAILED)
            toast_for(now, ERR_TOAST_MS, "wifi connect failed (retrying)");
        else if (app.prev_wifi == WIFI_MGR_CONNECTED)   /* -> LOST / IDLE */
            toast(now, "wifi disconnected");
        app.prev_wifi = w;
    }

    if (app.ble && app.ble->get_state) {
        cyberdeck_ble_state_t b = app.ble->get_state();
        if ((uint8_t)b != app.prev_ble) {
            if (b == CYBERDECK_BLE_CONNECTED) {
                const char *n = app.ble->get_name
                              ? app.ble->get_name() : "";
                toast(now, "keyboard connected %s", n);
            } else if (b == CYBERDECK_BLE_CONNECTING) {
                toast(now, "keyboard connecting...");
            } else if (app.prev_ble == CYBERDECK_BLE_CONNECTED &&
                       b != CYBERDECK_BLE_PAIRING_SCAN) {
                toast(now, "keyboard disconnected");
            }
            app.prev_ble = (uint8_t)b;
        }
    }
}

/* The whole surface at a glance, scr_id_t-indexed (designated, so array
 * order can't drift from the enum). Every screen — future plugins
 * included — is a line here; no runtime registration (extensibility.md
 * status log 2026-08-26). */
static const nav_screen_t *const SCREENS[SCR_COUNT] = {
    [SCR_BOOT]       = &boot_screen,
    [SCR_HOME]       = &home_screen,
    [SCR_POWEROFF]   = &poweroff_screen,
    [SCR_PAIRING]    = &pairing_screen,
    [SCR_HOSTKEY]    = &hostkey_screen,
    [SCR_CONNECTING] = &connecting_screen,
    [SCR_SESSION]    = &session_screen,
    [SCR_MENU]       = &menu_screen,
    [SCR_WIFIPROV]   = &wifiprov_screen,
    [SCR_PROFILE]    = &profile_screen,
    [SCR_SSHIMPORT]  = &sshimport_screen,
    [SCR_UNLOCK]     = &unlock_screen,
};

esp_err_t cyberdeck_app_init(const cyberdeck_app_config_t *cfg, uint64_t now_ms)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    memset(&app, 0, sizeof(app));
    app.cfg = *cfg;
    app.ble      = cyberdeck_service(CYBERDECK_SVC_BLE_KEYBOARD);
    app.presence = cyberdeck_service(CYBERDECK_SVC_PRESENCE);
    app_touch_cfg_t tc = { .scroll = true };
    storage_kv_load(cyberdeck_settings_ini, app_touch_section, app_touch_fields, &tc);
    app.touch_scroll = tc.scroll;
    app_touch_scroll_apply();
    app_settings_register_reset();
    saver_init(now_ms);    /* [saver] load + idle timer starts at boot */

    esp_err_t err = ui_init();
    if (err != ESP_OK) return err;

    load_profiles();
    kick_wifi();

    if (!nav_init(SCREENS, SCR_COUNT)) return ESP_FAIL;
    if (!nav_reset(SCR_BOOT, 0, now_ms)) return ESP_FAIL;

    /* Render-effect tunables: defaults overlaid with settings.ini [fx] (internal-DRAM
     * startup task — flash I/O is safe here). */
    display_fx_cfg_t fxc;
    display_fx_defaults(&fxc);
    storage_kv_load(cyberdeck_settings_ini, app_fx_section, app_fx_fields, &fxc);
    display_fx_set(&fxc);

    /* Plugins (plugin_table.c): settings load, then init.
     * cyberdeck_app_init logs and skips a failing plugin; the deck
     * boots without it. */
    for (int i = 0; i < cyberdeck_plugin_count; i++) {
        const cyberdeck_plugin_t *p = cyberdeck_plugins[i];
        if (p->settings && p->settings_obj)
            storage_kv_load(cyberdeck_settings_ini, p->name,
                            p->settings, p->settings_obj);
        if (p->init && p->init() != ESP_OK)
            ESP_LOGE(TAG, "plugin %s failed to init", p->name);
    }

    ESP_LOGI(TAG, "shell up: %d profile(s), %d plugin(s)",
             app.profile_count, cyberdeck_plugin_count);
    return ESP_OK;
}

bool cyberdeck_app_in_session(void)
{
    return nav_current() == SCR_SESSION || nav_current() == SCR_MENU;
}

#ifdef BUILD_SIMULATOR
const char *cyberdeck_app_debug_screen(void)
{
    return nav_current_name();
}

bool cyberdeck_app_debug_overlay_contains(const char *text)
{
    return ui_debug_contains(text);
}
#endif

void cyberdeck_app_tick(uint64_t now)
{
    app.anim_frame = (uint32_t)(now / ANIM_PERIOD_MS);
    ui_frame(app.anim_frame);   /* marquee clock for ui_tile */

    status_toasts(now);
    app_settings_idle_flush();   /* deferred settings saves, once held pages close */

    for (int i = 0; i < cyberdeck_plugin_count; i++) {
        const cyberdeck_plugin_t *p = cyberdeck_plugins[i];
        if (p->tick)       p->tick(now);
        if (p->idle_flush) p->idle_flush();
    }

    nav_frame(now);
}

void cyberdeck_app_handle_input(const cyberdeck_input_t *ev, uint64_t now)
{
    if (!ev) return;

    /* Any input feeds the idle timer; saver_on_input swallows a true
     * screensaver wake (see it for the just-went-up grace). */
    if (saver_on_input(now))
        return;

    char ch = 0;
    ui_key_t k = (ev->type == CYBERDECK_INPUT_KEY ||
                  ev->type == CYBERDECK_INPUT_HIDKEY)
                 ? decode_key(ev, &ch) : K_NONE;

    nav_dispatch_input(ev, k, ch, now);
}
