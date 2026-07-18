/*
 * cyberdeck_app.c — the shell's core: shared state, init, dispatch.
 *
 * State machine:
 *
 *   BOOT ── delay ──> HOME (profile picker) ──Enter──> CONNECTING ──> SESSION
 *            ^          │  'b'                  ^  ESC     │ ok          │
 *            │          v                       │          v             v
 *            │       PAIRING ──Enter/ESC──> HOME│      HOSTKEY ──trust──>│
 *            │                                  │      (TOFU prompt)     │
 *            └─────── session drop (no auto-reconnect) <────── F12 ──> MENU
 *
 * All shell UI lives in the display overlay layer (app_ui). The vterm cell
 * buffer belongs to the boot splash and the SSH session; the shell never
 * writes ANSI into it except to clear it when a session starts.
 *
 * Screen behavior lives in the per-screen modules (see app_internal.h for
 * the file map). This file owns the one shared state instance, decodes
 * input once, and dispatches tick/input to the module for app.state.
 */

#include "app_internal.h"
#include "app_screens.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "display_fx.h"
#include "wifi_manager.h"

static const char *TAG = "cyberdeck_app";

/* The one shared shell state instance (declared in app_internal.h). */
struct app_state app;

/* ------------------------------------------------------------ key decode */

static ui_key_t decode_key(const cyberdeck_input_t *ev, char *ch)
{
    const uint8_t *b = ev->buf;
    int len = ev->len;

    if (len == 1) {
        if (b[0] == 0x1B) return K_ESC;
        if (b[0] == '\r' || b[0] == '\n') return K_ENTER;
        if (b[0] == 0x08 || b[0] == 0x7F) return K_BACKSPACE;
        if (b[0] == 0x09) return K_TAB;
        if (b[0] >= 0x20 && b[0] < 0x7F) { if (ch) *ch = (char)b[0]; return K_CHAR; }
        return K_NONE;
    }
    if (len >= 3 && b[0] == 0x1B && (b[1] == '[' || b[1] == 'O')) {
        switch (b[2]) {
        case 'A': return K_UP;
        case 'B': return K_DOWN;
        case 'C': return K_RIGHT;
        case 'D': return K_LEFT;
        }
    }
    if (len == 5 && memcmp(b, "\x1b[24~", 5) == 0) return K_F12;
    return K_NONE;
}

/* ------------------------------------------------------------- profiles */

void load_profiles(void)
{
    app.profile_count = 0;
    int n = 0;
    if (storage_load_profiles(app.profiles, &n, MAX_PROFILES - 1) != ESP_OK)
        n = 0;
    app.profile_count = n;
    app.stored_count  = n;   /* real, on-flash profiles (before any synth below) */

    /* Synthesize "(default)" from the Kconfig fallback ONLY when profiles.ini
     * gave us nothing — otherwise a populated file gets padded with a
     * redundant extra entry. */
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
    if (app.sel >= app.profile_count) app.sel = app.profile_count ? app.profile_count - 1 : 0;
}

/* True if a keyboard is bonded (present in the BLE registry). */
bool ble_has_bond(void)
{
    if (!app.cfg.ble) return false;
    ble_device_info_t d[STORAGE_BLE_MAX];
    int n = 0;
    storage_ble_list(d, STORAGE_BLE_MAX, &n);
    return n > 0;
}

void kick_wifi(void)
{
    wifi_profile_t nets[STORAGE_WIFI_MAX];
    int n = 0;
    storage_wifi_load(nets, &n, STORAGE_WIFI_MAX);

    if (n == 0 && app.cfg.fallback_wifi_ssid && app.cfg.fallback_wifi_ssid[0]) {
        memset(&nets[0], 0, sizeof(nets[0]));
        snprintf(nets[0].ssid, sizeof(nets[0].ssid), "%s",
                 app.cfg.fallback_wifi_ssid);
        snprintf(nets[0].password, sizeof(nets[0].password), "%s",
                 app.cfg.fallback_wifi_password ? app.cfg.fallback_wifi_password : "");
        n = 1;
    }

    if (n > 0) {
        wifi_manager_connect(nets, n);
    } else {
        ESP_LOGW(TAG, "no WiFi profiles (wifi.ini empty, no fallback)");
    }
}

/* -------------------------------------------------------------- toasts */

void toast_for(uint64_t now, uint32_t ms, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(app.toast, sizeof(app.toast), fmt, ap);
    va_end(ap);
    app.toast_until = now + ms;
    app.toast_ok    = false;   /* only enter_session() garnishes with a ✓ */
}

/* ------------------------------------------------------------- dispatch */

typedef struct {
    void (*tick)(uint64_t now);
    void (*input)(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                  uint64_t now);
} screen_ops_t;

/* One row per app_state_t — the whole FSM surface at a glance. */
static const screen_ops_t SCREENS[ST_COUNT] = {
    [ST_BOOT]       = { boot_tick,       boot_input       },
    [ST_HOME]       = { home_tick,       home_input       },
    [ST_PAIRING]    = { pairing_tick,    pairing_input    },
    [ST_HOSTKEY]    = { hostkey_tick,    hostkey_input    },
    [ST_CONNECTING] = { connecting_tick, connecting_input },
    [ST_SESSION]    = { session_tick,    session_input    },
    [ST_POWEROFF]   = { poweroff_tick,   poweroff_input   },
    [ST_MENU]       = { menu_tick,       menu_input       },
    [ST_WIFIPROV]   = { wifiprov_tick,   wifiprov_input   },
    [ST_PROFILE]    = { profile_tick,    profile_input    },
    [ST_SSHIMPORT]  = { sshimport_tick,  sshimport_input  },
};

/* ---------------------------------------------------------- public API */

esp_err_t cyberdeck_app_init(const cyberdeck_app_config_t *cfg, uint64_t now_ms)
{
    if (!cfg) return ESP_ERR_INVALID_ARG;

    memset(&app, 0, sizeof(app));
    app.cfg = *cfg;
    app.pf_edit_idx  = -1;
    app.pf_key_sel   = -1;
    app.reorder_grab = -1;
    app.state = ST_BOOT;
    app.boot_until = now_ms + cfg->boot_delay_ms;
    app.last_input = now_ms;   /* idle timer starts at boot */
    boot_advance_tagline();

    esp_err_t err = ui_init();
    if (err != ESP_OK) return err;

    load_profiles();
    kick_wifi();

    /* Render-effect tunables: defaults overlaid with fx.ini (internal-DRAM
     * startup task — flash I/O is safe here). */
    display_fx_cfg_t fxc;
    display_fx_defaults(&fxc);
    storage_fx_load(&fxc);
    display_fx_set(&fxc);

    ESP_LOGI(TAG, "shell up: %d profile(s)", app.profile_count);
    return ESP_OK;
}

bool cyberdeck_app_in_session(void)
{
    return app.state == ST_SESSION || app.state == ST_MENU;
}

void cyberdeck_app_tick(uint64_t now)
{
    if (app.halted) return;

    app.anim_frame = (uint32_t)(now / ANIM_PERIOD_MS);
    ui_frame(app.anim_frame);   /* marquee clock for ui_tile */

    if (app.state < ST_COUNT && SCREENS[app.state].tick)
        SCREENS[app.state].tick(now);
}

void cyberdeck_app_handle_input(const cyberdeck_input_t *ev, uint64_t now)
{
    if (!ev || app.halted) return;

    /* Any input feeds the idle timer. Only input arriving while the rain
     * is ACTUALLY on screen is swallowed as a wake — and only once the
     * rain has been up for a moment: the main loop ticks before it drains
     * input, so a keypress aimed at a HOME that was visible milliseconds
     * ago must still act, not vanish into a wake. */
    app.last_input = now;
    if (app.saver_on) {
        app.saver_on = false;
        if (app.toast[0] && now >= app.toast_until) app.toast[0] = '\0';
        render_home();
        if (now - app.saver_since >= 1000)
            return;                    /* true wake: swallow the input */
        /* else: rain just went up — fall through and act on the event */
    }

    char ch = 0;
    ui_key_t k = (ev->type == CYBERDECK_INPUT_KEY)
                 ? decode_key(ev, &ch) : K_NONE;

    if (app.state < ST_COUNT && SCREENS[app.state].input)
        SCREENS[app.state].input(ev, k, ch, now);
}
