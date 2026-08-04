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
#include "app_ui.h"
#include "storage.h"

typedef enum {
    ST_BOOT = 0,
    ST_HOME,        /* profile picker + status                       */
    ST_PAIRING,     /* BLE keyboard scan list (modal)                */
    ST_HOSTKEY,     /* trust-on-first-use fingerprint prompt (modal) */
    ST_CONNECTING,  /* pending/armed SSH connect                     */
    ST_SESSION,     /* bytes flow to/from SSH                        */
    ST_POWEROFF,    /* CRT collapse playing over the dead session    */
    ST_MENU,        /* in-session overlay menu                       */
    ST_WIFIPROV,    /* SoftAP WiFi onboarding (modal)                */
    ST_PROFILE,     /* on-device profile editor (modal)              */
    ST_SSHIMPORT,   /* SoftAP + HTTP SSH-profile import (modal)      */
    ST_COUNT,
} app_state_t;

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

/* A page of finger-sized tiles laid out in a grid, with two-axis touch
 * hit-testing. Recomputed by each render_*() and saved for the tap handler.
 * All dimensions are in character cells. */
typedef struct {
    int x0, y0;        /* top-left cell of the grid            */
    int tw, th;        /* tile size in cells                   */
    int gx, gy;        /* gutter between tiles, in cells       */
    int ncols, nrows;  /* tiles per page                       */
    int count;         /* live tiles on this page (<= ncols*nrows) */
} tilegrid_t;

/* Decoded UI keys (core decodes once per event; screens get the result). */
typedef enum {
    K_NONE = 0, K_UP, K_DOWN, K_LEFT, K_RIGHT,
    K_ENTER, K_ESC, K_F12, K_CHAR, K_BACKSPACE, K_TAB,
} ui_key_t;

/* ------------------------------------------------- per-module state
 * One struct per screen module, owned and mutated by the named app_*.c
 * alone; collected below into the app_state composite. */

typedef struct {                    /* app_boot.c */
    uint64_t until;                 /* when the splash ends */
} boot_state_t;

typedef struct {                    /* app_home.c */
    int      sel;                   /* selected HOME tile                  */
    bool     kbd_bonded;            /* gates the "Pair keyboard" tile      */
    uint8_t  kon_idx;               /* Konami sequence progress            */
    uint64_t next_refresh;          /* next live-status re-render          */
    uint64_t poweroff_until;        /* ST_POWEROFF: when the collapse ends */
} home_state_t;

typedef struct {                    /* app_saver.c */
    uint64_t last_input;            /* any key/touch; drives the idle timer  */
    bool     on;                    /* rain actually on screen (not derived) */
    uint64_t since;                 /* when the rain went up (wake grace)    */
} saver_state_t;

#define PAIR_MAX  STORAGE_BLE_MAX

typedef struct {                    /* app_pairing.c */
    ble_device_info_t devs[PAIR_MAX];
    int      ndevs;
    int      sel;
    uint64_t last_poll;
    uint64_t last_activity;
    bool     forget_armed;          /* "Forget bonds" needs a 2nd tap */
} pair_state_t;

typedef struct {                    /* app_hostkey.c */
    bool     mismatch;              /* pinned key CHANGED (vs first contact)  */
    bool     armed;                 /* mismatch REPLACE needs a 2nd activation */
    uint8_t  arm_src;               /* what armed it: 1 = tap, 2 = Enter       */
    uint32_t frame0;                /* anim_frame at entry (decode reveal)     */
    int      sel;                   /* 0 = trust/replace, 1 = cancel           */
} hostkey_state_t;

typedef struct {                    /* app_connect.c */
    conn_profile_t active;          /* snapshot of the profile being
                                     * connected/connected: list mutations
                                     * must never redirect a live session  */
    bool     armed;                 /* render one frame, then connect     */
    bool     connecting;            /* async connect worker is running    */
    bool     cancelled;             /* user aborted the in-flight connect */
    uint64_t connect_at;            /* not before (auto-reconnect delay)  */
    uint64_t started;               /* when the in-flight attempt began   */
    int      attempt;               /* 0 = user-initiated, >0 = auto-retry # */
    char     pinned_fp[65];         /* fp to pass as expected_fp, "" = none */
    uint64_t session_start;         /* enter_session() time, for NO CARRIER */
} conn_state_t;

typedef struct {                    /* app_menu.c */
    int      sel;
    int      screen;                /* menu_screen_t: page of the menu tree */
    bool     from_home;             /* config opened from HOME (no session) */
    bool     armed;                 /* a destructive item needs a 2nd hit   */
    char     msg[48];               /* action result, shown under the tiles */
    uint64_t msg_until;             /* auto-clear time; 0 = sticky          */
    bool     msg_wifi;              /* live-track wifi_status_str()         */
    int      reorder_grab;          /* grabbed stored index, -1 = none      */
} menu_state_t;

typedef struct {                    /* app_profile.c */
    conn_profile_t draft;           /* profile being entered            */
    char     port[6];               /* port as text (parsed on save)    */
    int      field;                 /* focused field (pf_field_t)       */
    int      cursor;                /* caret within the focused field   */
    char     err[40];               /* inline validation error, "" = ok */
    int      edit_idx;              /* stored index being edited, -1 = new */
    char     orig_name[32];         /* name at edit entry (slot re-found
                                     * by name at save time)            */
    bool     return_menu;           /* editor was entered from the menu */
    char   (*keys)[STORAGE_KEY_ID_LEN];  /* SPIRAM, PF_KEY_MAX entries  */
    int      nkeys;
    int      key_sel;               /* index into keys, -1 = none       */
    char     key_type[24];          /* cached type of the selected key  */
} pf_state_t;

typedef struct {                    /* app_wifiprov.c */
    uint64_t done_at;               /* finish after CRED_SUCCESS (0 = unset) */
} prov_state_t;

typedef struct {                    /* app_sshimport.c */
    int      seen;                  /* count already acknowledged on screen */
    char     last[32];              /* snapshot of the last imported name   */
} import_state_t;

struct app_state {
    cyberdeck_app_config_t cfg;
    app_state_t state;

    conn_profile_t profiles[MAX_PROFILES];
    int  profile_count;
    int  stored_count;              /* profiles actually on flash (excl. synth) */

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

    /* per-module state (owned by the named module) */
    boot_state_t    boot;
    home_state_t    home;
    saver_state_t   saver;
    pair_state_t    pair;
    hostkey_state_t hostkey;
    conn_state_t    conn;
    menu_state_t    menu;
    pf_state_t      pf;
    prov_state_t    prov;
    import_state_t  imp;
};

extern struct app_state app;

/* ------------------------------------------------- core services (cyberdeck_app.c) */

/** (Re)load stored profiles into app.profiles (+ Kconfig fallback synth). */
void load_profiles(void);

/** True if a keyboard is bonded (present in the BLE registry). */
bool ble_has_bond(void);

/** Connect wifi_manager from wifi.ini (or the Kconfig fallback). */
void kick_wifi(void);

/** Post a toast for @p ms; drawn by HOME inline or the SESSION toast chip. */
void toast_for(uint64_t now, uint32_t ms, const char *fmt, ...);

/* Status trivia keeps the short default; errors the user must actually read
 * (auth failures, drop reasons) call toast_for() with ERR_TOAST_MS. */
#define toast(now, ...)  toast_for(now, TOAST_MS, __VA_ARGS__)
