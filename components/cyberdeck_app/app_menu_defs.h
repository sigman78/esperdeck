/*
 * app_menu_defs.h — the static menu tree (internal).
 *
 * Menu is a shallow tree of tile pages: a root (MAIN in-session), a CONFIG
 * hub, and topic submenus. HOME opens straight into CONFIG. Each page holds
 * few enough big tiles to fit the screen without scrolling. The definitions
 * live in app_menu_defs.c; rendering and actions in app_menu.c.
 */

#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"   /* CONFIG_INPUT_TOUCH_SCROLL (sim: -D flag) */
#endif

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    MS_MAIN = 0,   /* in-session root: Resume / Disconnect / Configuration    */
    MS_CONFIG,     /* hub: Profiles / WiFi / Keyboard / System / Back         */
    MS_PROFILES,   /* Add / Edit / Reorder / Delete / Import > / Back         */
    MS_IMPORT,     /* SoftAP (phone) / Web (PC) / Back                        */
    MS_WIFI,       /* Reconnect / Add network / Back                          */
    MS_KEYBOARD,   /* Pair / Forget bonds / Back                             */
    MS_SYSTEM,     /* Saver / Clear host keys / Factory reset / Back          */
    MS_EFFECTS,    /* dynamic bodies: every runtime render-fx tunable        */
    MS_FONT,       /* dynamic bodies: terminal font size, applied on reboot  */
    MS_KEYSTORE,   /* dynamic bodies: lock now / set code / lock triggers    */
    MS_DELPROFILE, /* dynamic: pick a stored profile to delete               */
    MS_EDITPROFILE,/* dynamic: pick a stored profile to edit                 */
    MS_REORDER,    /* dynamic: grab a profile, move it, drop it              */
} menu_screen_t;

#define CFG_KEYBOARD 2   /* MS_CONFIG index of "Keyboard >" (needs BLE) */
#define CFG_KEYSTORE 5   /* MS_CONFIG index of "Keystore >" (excludable) */

/* MS_SYSTEM tile indices. Named because the page mixes harmless tunables
 * with the two destructive actions, and menu_confirm() picks those out by
 * index — inserting a tile above them without moving the confirm gate would
 * arm "factory reset" from the wrong tile. */
typedef enum {
    SYS_SAVER = 0,
#if CONFIG_INPUT_TOUCH_SCROLL
    SYS_TOUCHSCROLL,
#endif
    SYS_CLEARHOSTS,
    SYS_FACTORY,
    SYS_BACK,
    SYS_MENU_TILES,     /* count — keeps the page array sized with the enum */
} sys_menu_item_t;

typedef struct {
    const char        *title;
    const char *const *items;
    const uint8_t     *cols;
    int                count;
} menu_def_t;

/** Static definition for a screen; the profile pickers are built dynamically. */
menu_def_t menu_def(int sc);

/** True for the dynamically built stored-profile pickers. */
bool menu_is_picker(int sc);

/** CONFIRM label if (sc,sel) is a destructive 2-step action, else NULL. */
const char *menu_confirm(int sc, int sel);

/** Is (sc,sel) unavailable because BLE support is absent? */
bool menu_item_dim(int sc, int sel);
