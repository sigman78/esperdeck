/*
 * app_internal.h — the shell's shared spine (internal to cyberdeck_app).
 *
 * One state struct, one instance (`app`, defined in cyberdeck_app.c), shared
 * by the per-screen modules:
 *
 *   cyberdeck_app.c   core: init, tick/input dispatch, profile + wifi services
 *   app_widgets.[ch]  tile grid + shared chrome (titlebar, footer, QR, ...)
 *   app_menu_defs.[ch] the static menu tree (pages, items, colors, predicates)
 *   app_boot.c        boot splash            app_home.c     HOME + POWEROFF
 *   app_saver.c       idle rain screensaver  app_pairing.c  BLE pairing modal
 *   app_hostkey.c     TOFU host-key modal    app_connect.c  CONNECTING + SESSION
 *   app_profile.c     profile editor         app_menu.c     menu pages + actions
 *   app_wifiprov.c    SoftAP wifi onboarding app_sshimport.c HTTP profile import
 *
 * Screen modules export their enter/render entries plus a <screen>_tick and
 * <screen>_input pair (declared in app_screens.h) that the core dispatches
 * on app.state.
 */

#pragma once

#include "cyberdeck_app.h"
#include "app_ui.h"
#include "storage.h"

/* ---------------------------------------------------------------- state */

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

/* HOME trailing tiles after the profile tiles. "New profile" only appears as a
 * first-run shortcut when nothing is stored yet (otherwise profiles are added
 * from Config); "Pair keyboard" only when no keyboard is bonded. Configuration
 * is always present and always last. Order is resolved by home_extras(). */
typedef enum { HX_NEW, HX_PAIR, HX_CONFIG } home_extra_t;
#define HOME_EXTRA_MAX 3
#define PAIR_MAX         STORAGE_BLE_MAX
#define PAIR_TIMEOUT_MS  30000
#define PAIR_POLL_MS     250
#define HOME_REFRESH_MS  500
#define ANIM_PERIOD_MS   100          /* ~10 fps subtle UI animation */
#define TOAST_MS         3000         /* status trivia */
#define ERR_TOAST_MS     7000         /* errors the user must actually read */
#define MENU_MSG_MS      5000         /* menu action feedback lifetime */
#define SAVER_IDLE_MS    (3 * 60 * 1000)  /* HOME idle before the rain */
#define PROV_ACK_HOLD_MS 2500         /* wifiprov success hold (phone ack) */

/* Shell palette — VGA phosphor green on black. Per-cell accents (OVERLAY_COL_*)
 * layer the rest of the classic 16-color set on top. */
#define UI_FG   RGB565(85, 255, 85)   /* VGA bright green */
#define UI_BG   RGB565(0, 0, 0)       /* VGA black        */

#define NELEM(a) ((int)(sizeof(a) / sizeof((a)[0])))

/* A page of finger-sized tiles laid out in a grid, with two-axis touch
 * hit-testing. Recomputed by each render_*() and saved for the tap handler.
 * All dimensions are in 8x16-px character cells. */
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

struct app_state {
    cyberdeck_app_config_t cfg;
    app_state_t state;

    conn_profile_t profiles[MAX_PROFILES];
    int  profile_count;
    int  stored_count;              /* profiles actually on flash (excl. synth) */
    bool kbd_bonded;                /* a keyboard is in the BLE registry */
    int  sel;                       /* HOME tile selection */

    /* Tile grid of the current screen, saved for touch hit-testing. */
    tilegrid_t grid;

    /* connecting */
    conn_profile_t active;          /* snapshot of the profile being
                                     * connected/connected: list mutations
                                     * (edit/delete/reorder) must never
                                     * redirect a live session            */
    int      connect_idx;           /* HOME slot the connect started from */
    bool     connect_armed;         /* render one frame, then connect     */
    bool     connecting;            /* async connect worker is running    */
    bool     connect_cancelled;     /* user aborted the in-flight connect */
    uint64_t connect_at;            /* not before (auto-reconnect delay)  */
    uint64_t connect_started;       /* when the in-flight attempt began   */
    int      connect_attempt;       /* 0 = user-initiated, >0 = auto-retry # */
    char     pinned_fp[65];         /* fp to pass as expected_fp, "" = none */

    /* hostkey prompt */
    bool     fp_mismatch;
    bool     hostkey_armed;         /* mismatch REPLACE needs a 2nd tap   */
    uint8_t  hostkey_arm_src;       /* what armed it: 1 = tap, 2 = Enter  */
    uint32_t hostkey_frame0;        /* anim_frame at entry (decode reveal) */
    int      hostkey_sel;           /* 0 = trust/replace, 1 = cancel      */

    /* pairing */
    ble_device_info_t devs[PAIR_MAX];
    int      ndevs;
    int      pair_sel;
    uint64_t pair_last_poll;
    uint64_t pair_last_activity;
    bool     pair_forget_armed;     /* "Forget bonds" needs a 2nd tap */

    /* menu */
    int  menu_sel;
    int  menu_screen;      /* menu_screen_t: which page of the menu tree */
    bool menu_from_home;   /* config opened from HOME (no session) */
    bool menu_armed;       /* a destructive item needs a 2nd activation */
    char menu_msg[48];     /* last action result, shown under the tiles */
    uint64_t menu_msg_until; /* auto-clear time; 0 = sticky (armed confirm) */
    bool menu_msg_wifi;    /* live-track wifi_status_str() while shown */

    /* wifi provisioning */
    uint64_t prov_done_at; /* when to finish after CRED_SUCCESS (0 = not set) */

    /* ssh-profile import over WiFi */
    int      import_seen;  /* ssh_import_count() already acknowledged on screen */
    char     import_last[32]; /* snapshot of the last imported name (stable) */

    /* toast (SESSION only; UI states draw status inline) */
    char     toast[64];
    uint64_t toast_until;
    bool     toast_ok;     /* success toast: spinner-to-checkmark garnish */

    uint64_t session_start;         /* enter_session() time, for NO CARRIER */
    uint64_t last_input;            /* any key/touch; drives the screensaver */
    bool     saver_on;              /* rain actually on screen (not derived) */
    uint64_t saver_since;           /* when the rain went up (wake grace)    */
    uint8_t  kon_idx;               /* Konami sequence progress (HOME)       */

    /* on-device profile editor */
    conn_profile_t pf_draft;        /* profile being entered           */
    char     pf_port[6];            /* port as text (parsed on save)   */
    int      pf_field;              /* focused field (see pf_field_t)   */
    int      pf_cursor;             /* caret within the focused field   */
    char     pf_err[40];            /* inline validation error, "" = ok */
    int      pf_edit_idx;           /* stored index being edited, -1 = new */
    char     pf_orig_name[32];      /* name at edit entry (slot re-found
                                     * by name at save time)            */
    bool     pf_return_menu;        /* editor was entered from the menu */
    char   (*pf_keys)[STORAGE_KEY_ID_LEN];  /* SPIRAM, PF_KEY_MAX entries */
    int      pf_nkeys;
    int      pf_key_sel;            /* index into pf_keys, -1 = none    */
    char     pf_key_type[24];       /* cached type of the selected key  */

    /* reorder picker */
    int      reorder_grab;          /* grabbed stored index, -1 = none  */

    uint64_t boot_until;
    uint64_t next_home_refresh;
    uint32_t anim_frame;            /* advances ~10 fps for subtle animation */
    uint64_t next_anim;             /* next animated re-render (PAIRING)      */
    uint64_t poweroff_until;        /* ST_POWEROFF: when the collapse ends    */
    bool     halted;
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
