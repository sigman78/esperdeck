/*
 * app_menu_defs.h — the data-driven menu tree (internal).
 *
 * A page is a table of menu_item_t: behavior (action / confirm / dim /
 * value) rides the table, not a positional switch. Tables and their
 * action callbacks live in app_menu_defs.c; rendering, input and the
 * dynamic profile pickers in app_menu.c. (extensibility.md item 3)
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    MS_MAIN = 0,   /* in-session root: Resume / Disconnect / Configuration    */
    MS_CONFIG,     /* hub: Profiles / WiFi / Keyboard / System / Back         */
    MS_PROFILES,   /* Add / Edit / Reorder / Delete / Import > / Back         */
    MS_IMPORT,     /* SoftAP (phone) / Web (PC) / Back                        */
    MS_WIFI,       /* Reconnect / Add network / Back                          */
    MS_KEYBOARD,   /* Pair / Forget bonds / Back                             */
    MS_SYSTEM,     /* Saver / Clear host keys / Factory reset / Back          */
    MS_EFFECTS,    /* value tiles: every runtime render-fx tunable           */
    MS_FONT,       /* value tiles: terminal font size, applied on reboot     */
    MS_KEYSTORE,   /* contextual: lock now / set code / remove code          */
    MS_DELPROFILE, /* dynamic: pick a stored profile to delete               */
    MS_EDITPROFILE,/* dynamic: pick a stored profile to edit                 */
    MS_REORDER,    /* dynamic: grab a profile, move it, drop it              */
} menu_screen_t;

/* One tile. Static parts inline; dynamic parts are callbacks taking the
 * item's @p arg, so one function serves a family of items (font sizes,
 * fx tunables). Only label and one of color/color_fn are required. */
typedef struct {
    const char *label;
    const char *(*label_fn)(intptr_t arg);       /* overrides label      */
    uint8_t     color;                           /* OVERLAY_COL_*        */
    uint8_t   (*color_fn)(intptr_t arg);         /* overrides color      */
    intptr_t    arg;
    void      (*action)(intptr_t arg, uint64_t now);
    const char *confirm;       /* 2-step: tile label while armed         */
    const char *arm_note;      /* 2-step: feedback on the arming hit     */
    bool      (*hidden)(intptr_t arg);           /* tile not rendered    */
    bool      (*dim)(intptr_t arg);              /* shown, unavailable   */
    const char *dim_note;      /* feedback for activating a dim item     */
    const char *(*value)(intptr_t arg, char *buf, size_t sz);  /* body   */
} menu_item_t;

enum {
    MENU_PAGE_WIDE = 1 << 0,   /* multi-column picker_grid layout        */
    MENU_PAGE_VALS = 1 << 1,   /* value right-aligned on the title row   */
};

#define MENU_BACK_LEAVE (-1)   /* back_to: leave the menu screen (pop)   */

typedef struct {
    const char *title;
    const menu_item_t *items;
    uint8_t count;
    int8_t  back_to;           /* menu_screen_t target, or MENU_BACK_LEAVE */
    uint8_t flags;             /* MENU_PAGE_*                            */
    void  (*on_open)(void);    /* snapshot volatile state at page open   */
} menu_page_t;

/* Largest items[] count across the pages (slot-map sizing in app_menu.c;
 * checked against the tables by a static assert in app_menu_defs.c). */
#define MENU_MAX_TILES 12

/** The page table for @p sc, or NULL for the dynamic pickers. */
const menu_page_t *menu_page(int sc);

/** True for the dynamically built stored-profile pickers. */
bool menu_is_picker(int sc);

/* app_menu.c internals the tables call back into. Shell-wide callers use
 * menu_goto/menu_note (app_screens.h); these stay menu-private. */
void menu_back(uint64_t now);
void menu_present_now(uint64_t now);   /* paint + present before a reboot */
