/*
 * app_screens.h — per-screen module surface (internal).
 *
 * Each module exports its nav_screen_t hook table; cyberdeck_app.c
 * registers them all and owns the SCR_* ids. Cross-screen jumps go
 * through the semantic entry points below — thin wrappers that set up
 * intent state and call nav_push/replace/reset.
 */

#pragma once

#include "app_internal.h"
#include "ssh_import.h"     /* ssh_import_mode_t */

/* Screen ids, assigned at registration (cyberdeck_app.c). */
extern int SCR_BOOT, SCR_HOME, SCR_POWEROFF, SCR_PAIRING, SCR_HOSTKEY,
           SCR_CONNECTING, SCR_SESSION, SCR_MENU, SCR_WIFIPROV,
           SCR_PROFILE, SCR_SSHIMPORT, SCR_UNLOCK;

/* ---- boot (app_boot.c) ---- */
extern const nav_screen_t boot_screen;

/* ---- HOME + POWEROFF (app_home.c) ---- */
extern const nav_screen_t home_screen, poweroff_screen;
/** Land on HOME (collapses the nav stack — HOME is always the bottom). */
void enter_home(uint64_t now);
/** Session teardown entry: CRT collapse over the dead frame, then HOME. */
void enter_home_after_collapse(uint64_t now);

/* ---- screensaver (app_saver.c) ---- */
/** Count activity: restart the idle timer, rain off. */
void saver_reset(uint64_t now);
/** Feed an input event; true = it woke the rain and must be swallowed. */
bool saver_on_input(uint64_t now);
/** Run the rain when HOME is idle; true = this tick was handled. */
bool saver_tick_home(uint64_t now);
/** Idle rain over the DEVICE gate pad; true while the rain owns the screen. */
bool saver_tick_gate(uint64_t now);

/* ---- pairing (app_pairing.c) ---- */
extern const nav_screen_t pairing_screen;
void enter_pairing(uint64_t now);

/* ---- hostkey TOFU modal (app_hostkey.c) ---- */
extern const nav_screen_t hostkey_screen;
/** Enter the prompt for the fingerprint ssh_client just reported. */
void hostkey_open(bool mismatch, uint64_t now);

/* ---- unlock (app_unlock.c) ---- */
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

/* ---- connect + session (app_connect.c) ---- */
extern const nav_screen_t connecting_screen, session_screen;
/** Arm a connect to profile @p idx (snapshots it into app.conn.active). */
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

/* ---- menu (app_menu.c) ---- */
extern const nav_screen_t menu_screen;
/** Open the in-session root menu (F12 / long-press). */
void menu_open(uint64_t now);
/** Open the config hub directly from HOME (no session behind it). */
void menu_open_config(uint64_t now);
/** Switch to menu screen @p sc (menu_screen_t), resetting selection/arm. */
void menu_goto(int sc);
void menu_fx_flush(void);   /* deferred [fx] save — call from the app tick */
/** Discard a grabbed-but-not-dropped reorder (session drop safety). */
void menu_abort_reorder(void);
/** Post an action-feedback line under the menu tiles (0 ms = sticky). */
void menu_note(uint64_t now, uint32_t ms, bool live_wifi, const char *text);

/* ---- wifi provisioning (app_wifiprov.c) ---- */
extern const nav_screen_t wifiprov_screen;
void enter_wifiprov(uint64_t now);

/* ---- ssh profile import (app_sshimport.c) ---- */
extern const nav_screen_t sshimport_screen;
void enter_sshimport(uint64_t now, ssh_import_mode_t mode);
