/*
 * app_screens.h — per-screen module surface (internal).
 *
 * Each module exports its nav_screen_t hook table; the id-indexed
 * SCREENS[] table in cyberdeck_app.c maps them. Cross-screen jumps go
 * through the semantic entry points below — thin wrappers that set up
 * intent state and call nav_push/replace/reset.
 */

#pragma once

#include "app_internal.h"
#include "ssh_import.h"     /* ssh_import_mode_t */

/* Screen ids — indices into SCREENS[] (cyberdeck_app.c). One
 * compile-time table for every screen, future plugins included; a
 * compiled-out feature stub-swaps its descriptor, never its id. */
typedef enum {
    SCR_BOOT, SCR_HOME, SCR_POWEROFF, SCR_PAIRING, SCR_HOSTKEY,
    SCR_CONNECTING, SCR_SESSION, SCR_MENU, SCR_WIFIPROV,
    SCR_PROFILE, SCR_SSHIMPORT, SCR_UNLOCK, SCR_SAVER, SCR_HOST, SCR_ASK,
    SCR_WIFINET,
    SCR_COUNT
} scr_id_t;

extern const nav_screen_t boot_screen;

extern const nav_screen_t home_screen, poweroff_screen;
/** Land on HOME (collapses the nav stack — HOME is always the bottom). */
void enter_home(uint64_t now);
/** Session teardown entry: CRT collapse over the dead frame, then HOME. */
void enter_home_after_collapse(uint64_t now);
/** Connect to profile @p idx, or toast when wifi is down. HOME and the
 *  host page share it. */
void connect_checked(int idx, uint64_t now);

/* ---- host page (app_host.c) ---- */
extern const nav_screen_t host_screen;
/** Open the page of profile @p idx (stored or the built-in fallback). */
void enter_host(uint64_t now, int idx);
/** Delete stored profile @p idx with its key files; the host pin goes
 *  too unless @p keep_pin. Reloads the profile list. */
void profile_delete_at(int idx, bool keep_pin);
/** True when stored profile @p idx signs in with a key that no other
 *  stored profile uses. Deleting the profile deletes that key's files. */
bool profile_key_unshared(int idx);

/* ---- saved-network page (app_wifinet.c) ---- */
extern const nav_screen_t wifinet_screen;
/** Open the page of the saved network at position @p idx. */
void enter_wifinet(uint64_t now, int idx);

/* ---- question page (app_ask.c) ---- */
typedef struct {
    const char *title;            /* "Delete nas?"; copied                 */
    const char *line1, *line2;    /* what happens; copied; may be NULL     */
    const char *verb;             /* the confirming button: "Delete"       */
    uint8_t     accent;           /* OVERLAY_COL_*; 0 = red                */
    void      (*yes)(intptr_t arg, uint64_t now);
    intptr_t    arg;
    /* Optional second answer, a button between Cancel and the verb.
     * Cancel and Esc still only go back. */
    const char *no_verb;
    void      (*no)(intptr_t arg, uint64_t now);
} ask_t;
extern const nav_screen_t ask_screen;
/** Push the question page. The verb pops it, then calls yes(arg, now);
 *  the second answer calls no(arg, now) the same way when set. */
void ask_open(const ask_t *q, uint64_t now);

/* ---- screensaver (app_saver.c, plugin) ---- */
/** The rain screen; the saver plugin pushes it when session_guard says
 *  the deck idles on HOME or the gate pad. Any input pops it. */
extern const nav_screen_t saver_screen;

extern const nav_screen_t pairing_screen;
void enter_pairing(uint64_t now);

/* ---- hostkey TOFU modal (app_hostkey.c) ---- */
extern const nav_screen_t hostkey_screen;
/** Enter the prompt for the fingerprint ssh_client just reported. */
void hostkey_open(bool mismatch, uint64_t now);

extern const nav_screen_t unlock_screen;
/** Keystore PIN pad. @p resume_connect: re-arm the connect to
 *  app.conn.active once unlocked (the lazy on-first-key-use trigger). */
void unlock_open(uint64_t now, bool resume_connect);
/** Set-code flow from the menu: create (store absent) or change. */
void unlock_open_setpin(uint64_t now);
/** Remove-keystore flow: prove the code, unwrap keys to plaintext. */
void unlock_open_remove(uint64_t now);
/** DEVICE gate pad (boot/wake/Lock deck): non-skippable, rain-capable. */
void unlock_open_gate(uint64_t now);
/** True while the pad on screen is the non-skippable device gate (and
 *  not mid-derive) — the saver plugin may rain over it. */
bool unlock_is_gate(void);

extern const nav_screen_t connecting_screen, session_screen;
/** The profile snapshot that is connecting or already connected
 *  (hostkey prompt). */
const conn_profile_t *conn_active(void);
/** session_enter() time — the menu's UP clock. */
uint64_t conn_session_start(void);
/** Wipe the snapshot's secret (app_creds_wipe companion). */
void conn_creds_wipe(void);
/** Arm a connect to profile @p idx (snapshots it into the conn state). */
void start_connect(int idx, uint64_t not_before, uint64_t now);
/** Re-arm a connect to the ACTIVE snapshot (unlock-screen resume). */
void connect_resume_active(uint64_t now);
/** Re-arm a connect to the active snapshot, pinning @p fp (hostkey trust). */
void connect_arm_pinned(const char *fp, uint64_t now);
/** Session died and we are NOT auto-reconnecting: NO CARRIER + collapse. */
void session_dropped(uint64_t now);
/** Note scrollback activity: raises the right-edge indicator and restarts
 *  its linger timer. Call after any change to the scroll offset. */
void session_scroll_seen(uint64_t now);

/* ---- profile editor (app_profile.c) ---- */
extern const nav_screen_t profile_screen;
/** Open the editor: @p edit_idx = stored profile to edit, -1 = new. */
void enter_profile(uint64_t now, int edit_idx);
/** Wipe the draft's secret (app_creds_wipe companion). */
void profile_creds_wipe(void);

extern const nav_screen_t menu_screen;
/** Open the in-session root menu (F12 / long-press). */
void menu_open(uint64_t now);
/** Open the config hub directly from HOME (no session behind it). */
void menu_open_config(uint64_t now);
/** Switch to menu screen @p sc (menu_screen_t), resetting selection/arm. */
void menu_goto(int sc);
/** Post an action-feedback line under the menu tiles (0 ms = sticky). */
void menu_note(uint64_t now, uint32_t ms, bool live_wifi, const char *text);

/* ---- wifi provisioning (app_wifiprov.c) ---- */
extern const nav_screen_t wifiprov_screen;
void enter_wifiprov(uint64_t now);

/* ---- ssh profile import (app_sshimport.c) ---- */
extern const nav_screen_t sshimport_screen;
void enter_sshimport(uint64_t now, ssh_import_mode_t mode);
