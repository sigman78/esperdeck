/*
 * app_screens.h — per-screen module entry points (internal).
 *
 * Each app.state has a <screen>_tick(now) driven from cyberdeck_app_tick()
 * and a <screen>_input(ev, k, ch, now) driven from
 * cyberdeck_app_handle_input() after key decode. Cross-screen jumps go
 * through the enter/open functions.
 */

#pragma once

#include "app_internal.h"
#include "ssh_import.h"     /* ssh_import_mode_t */

/* ---- boot (app_boot.c) ---- */
void boot_advance_tagline(void);        /* next splash tagline (init-time) */
void boot_tick(uint64_t now);
void boot_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now);

/* ---- HOME (app_home.c) ---- */
void enter_home(uint64_t now);
/** Session teardown entry: HOME melts down over the dead frame. */
void enter_home_after_melt(uint64_t now);
void render_home(void);
void home_tick(uint64_t now);
void home_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now);

/* ---- screensaver (app_saver.c) ---- */
void render_saver(void);

/* ---- pairing (app_pairing.c) ---- */
void enter_pairing(uint64_t now);
void pairing_tick(uint64_t now);
void pairing_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now);

/* ---- hostkey TOFU modal (app_hostkey.c) ---- */
/** Enter the prompt for the fingerprint ssh_client just reported. */
void hostkey_open(bool mismatch);
void hostkey_tick(uint64_t now);
void hostkey_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now);

/* ---- connect + session (app_connect.c) ---- */
/** Arm a connect to profile @p idx (snapshots it into app.active). */
void start_connect(int idx, uint64_t not_before, uint64_t now);
void render_connecting(const char *msg, uint64_t now);
/** Session died and we are NOT auto-reconnecting: NO CARRIER + collapse. */
void session_dropped(uint64_t now);
void connecting_tick(uint64_t now);
void connecting_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now);
void session_tick(uint64_t now);
void session_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now);

/* ---- profile editor (app_profile.c) ---- */
/** Open the editor: @p edit_idx = stored profile to edit, -1 = new. */
void enter_profile(uint64_t now, int edit_idx);
void profile_tick(uint64_t now);
void profile_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now);

/* ---- menu (app_menu.c) ---- */
/** Open the in-session root menu (F12 / long-press). */
void menu_open(uint64_t now);
/** Switch to menu screen @p sc (menu_screen_t), resetting selection/arm. */
void menu_goto(int sc);
/** Post an action-feedback line under the menu tiles (0 ms = sticky). */
void menu_note(uint64_t now, uint32_t ms, bool live_wifi, const char *text);
void menu_tick(uint64_t now);
void menu_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now);

/* ---- wifi provisioning (app_wifiprov.c) ---- */
void enter_wifiprov(uint64_t now);
void wifiprov_tick(uint64_t now);
void wifiprov_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now);

/* ---- ssh profile import (app_sshimport.c) ---- */
void enter_sshimport(uint64_t now, ssh_import_mode_t mode);
void sshimport_tick(uint64_t now);
void sshimport_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now);
