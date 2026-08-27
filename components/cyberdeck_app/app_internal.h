/*
 * app_internal.h — the shell's shared spine (internal to cyberdeck_app).
 *
 * One state instance (`app`, defined in cyberdeck_app.c), composed of
 * per-module state structs: each screen module owns and mutates its own
 * member (app.pair, app.conn, ...); the core owns the rest. Cross-module
 * entry points are declared in app_screens.h.
 */

#pragma once

#include "cyberdeck_app.h"
#include "app_nav.h"
#include "app_ui.h"
#include "storage.h"

#define MAX_PROFILES     (STORAGE_MAX_PROFILES + 1)   /* stored + synth fallback */

#define ANIM_PERIOD_MS   100          /* ~10 fps subtle UI animation */
#define TOAST_MS         3000         /* status trivia */
#define ERR_TOAST_MS     7000         /* errors the user must actually read */
#define MENU_MSG_MS      5000         /* menu action feedback lifetime */

/* Shell palette — VGA phosphor green on black. Per-cell accents
 * (OVERLAY_COL_*) layer the rest of the classic 16 on top. */
#define UI_FG   RGB565(85, 255, 85)
#define UI_BG   RGB565(0, 0, 0)

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* tilegrid_t moved to the public kit (cyberdeck_ui.h, via app_ui.h). */

/** Push app.touch_scroll down to the touch driver (no-op without the
 *  gesture compiled in). Call after boot load and after every toggle. */
void app_touch_scroll_apply(void);

/* The shared core. Per-screen state lives with its module (file-static
 * behind the nav hooks); what remains here is genuinely cross-cutting. */
struct app_state {
    cyberdeck_app_config_t cfg;

    conn_profile_t profiles[MAX_PROFILES];
    int  profile_count;
    int  stored_count;              /* profiles actually on flash (excl. synth) */

    /* Right-edge scroll drag, toggled from SYSTEM and stored in [touch].
     * Kept even when CONFIG_INPUT_TOUCH_SCROLL is off so the field's absence
     * never has to be #ifdef'd at every read site. */
    bool touch_scroll;

    /* Tile grid of the current screen, saved for touch hit-testing. */
    tilegrid_t grid;

    /* toast (SESSION only; UI states draw status inline) */
    char     toast[64];
    uint64_t toast_until;
    bool     toast_ok;     /* success toast: spinner-to-checkmark garnish */

    /* link watcher: last states seen, for connect/disconnect toasts */
    uint8_t  prev_wifi;    /* wifi_mgr_state_t */
    uint8_t  prev_ble;     /* ble_state_t as int (see cyberdeck_ble_ops_t) */

    uint32_t anim_frame;   /* advances ~10 fps for subtle animation */
    uint64_t next_anim;    /* next animated re-render */

};

extern struct app_state app;

/* ------------------------------------------------- core services (cyberdeck_app.c) */

/** (Re)load stored profiles into app.profiles (+ Kconfig fallback synth). */
void load_profiles(void);

/**
 * Scrub the plaintext credentials load_profiles() hydrated into app state.
 * Call it with every keystore_lock() — that one wipes the master key and
 * the secrets cache INSIDE the vault, and knows nothing about the copies
 * out here. Metadata survives on purpose (HOME renders locked).
 */
void app_creds_wipe(void);

/** True if a keyboard is bonded (present in the BLE registry). */
bool ble_has_bond(void);

/** Connect wifi_manager from wifi.ini (or the Kconfig fallback). */
void kick_wifi(void);
void wifi_migrate_nvs_cred(void);

/** Post a toast for @p ms; drawn by HOME inline or the SESSION toast chip. */
void toast_for(uint64_t now, uint32_t ms, const char *fmt, ...);

/* Status trivia keeps the short default; errors the user must actually read
 * (auth failures, drop reasons) call toast_for() with ERR_TOAST_MS. */
#define toast(now, ...)  toast_for(now, TOAST_MS, __VA_ARGS__)
