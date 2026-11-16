/*
 * app_menu_defs.h - the setup tree as data (internal).
 *
 * A section is a table of menu_item_t. An item is an action, a switch
 * or a choice, and it carries its own behavior. Tables and callbacks
 * live in app_menu_defs.c; drawing and input live in app_menu.c.
 * Look and layout: docs/ui-kit.md, sketches mo-setup and mo-section.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cyberdeck_ui.h"   /* ui_part_t */

typedef enum {
    MS_MAIN = 0,   /* in-session sheet: Reset terminal, Setup, Disconnect */
    MS_CONFIG,     /* the Setup hub: one tile per section                 */
    MS_HOSTS,      /* new host, import, the stored hosts                  */
    MS_WIFI,       /* link facts, Reconnect, Add from phone, saved networks */
    MS_KEYBOARD,   /* Pair, Forget bonds                                  */
    MS_FX_CRT,     /* beam look: scanlines, phosphor, bold                */
    MS_FX_MOTION,  /* motion: wobble, wipe, collapse, static              */
    MS_SYSTEM,     /* font size, saver, edge scroll, clear host keys, factory reset */
    MS_KEYSTORE,   /* contextual: lock, set or change code, remove code   */
    MS_COUNT
} menu_screen_t;

/* How an item draws and what a hit means. */
typedef enum {
    MENU_ACT = 0,  /* ui_part: a hit runs `action`                        */
    MENU_SWITCH,   /* ui_switch: `on` reads the state, `action` flips it  */
    MENU_PICK,     /* ui_chips: every option shows; a hit runs `set`      */
} menu_kind_t;

/* One item. Callbacks take the item's @p arg, so one function serves a
 * family (fx tunables). State callbacks run every render (about 10 Hz):
 * keep them cheap and snapshot slow reads in the page's on_open. */
typedef struct {
    uint8_t     kind;                            /* menu_kind_t           */
    const char *label;
    const char *(*label_fn)(intptr_t arg);       /* overrides label       */
    intptr_t    arg;
    bool        nav;           /* opens another page: draws a chevron     */
    uint8_t     accent;        /* OVERLAY_COL_*; red marks a destructive act */
    bool        danger;        /* destructive: set apart at the page bottom */
    void      (*action)(intptr_t arg, uint64_t now);
    bool      (*on)(intptr_t arg);               /* MENU_SWITCH state     */
    int       (*count)(intptr_t arg);            /* MENU_PICK: options, at most MENU_PICK_MAX */
    int       (*index)(intptr_t arg);            /* MENU_PICK: chosen one */
    const char *(*option)(intptr_t arg, int i);  /* MENU_PICK: label      */
    void      (*set)(intptr_t arg, int i, uint64_t now);
    const char *ask;           /* MENU_ACT: question page title; the verb */
    const char *ask_line;      /* is the label. NULL runs `action` at once */
    bool      (*hidden)(intptr_t arg);           /* item not drawn        */
    bool      (*off)(intptr_t arg);              /* drawn, not a target   */
} menu_item_t;

#define MENU_PICK_MAX   8      /* options of one choice                   */
#define MENU_ITEM_MAX   8      /* items of one section                    */
#define MENU_BACK_LEAVE (-1)   /* back pops the whole menu screen         */

/* Rows of data under a section's items, drawn as tiles. The items repeat
 * on every page of tiles, and danger items hang below the last. Count,
 * row and off run every render. */
typedef struct {
    int  (*count)(void);
    void (*row)(int i, ui_part_t *out, char *buf, size_t sz);  /* tile of row i */
    void (*open)(int i, uint64_t now);       /* a tap or Enter on row i   */
    bool (*off)(void);                       /* all rows drawn off        */
    const char *empty;                       /* shown when count is 0     */
    int  min_w;                              /* narrowest tile, cells     */
} menu_list_t;

/* A text panel above a section's items. `fill` writes `rows` lines (at
 * most MENU_PANEL_MAX), keeps their text in @p txt and returns the stripe
 * accent, or 0. It runs every render. */
#define MENU_PANEL_MAX 4
#define MENU_PANEL_TXT 48
typedef struct {
    int     rows;
    uint8_t (*fill)(ui_info_row_t *out, char (*txt)[MENU_PANEL_TXT]);
} menu_panel_t;

/* One section. On the hub its tile shows `title` and the present state
 * from `note`. A non-NULL `unavail` result draws the tile off, with
 * that text as its detail. Both run every render. */
typedef struct {
    const char *title;
    const menu_item_t *items;
    const menu_panel_t *panel; /* text panel above the items, or NULL      */
    const menu_list_t *list;   /* tiles below the items, or NULL           */
    uint8_t count;
    int8_t  back_to;           /* menu_screen_t target, or MENU_BACK_LEAVE */
    uint8_t hold;              /* APP_SETTINGS_HOLD_* while the page is open */
    void  (*on_open)(void);    /* snapshot volatile state at page open    */
    const char *(*note)(char *buf, size_t sz);
    const char *(*unavail)(void);
} menu_page_t;

/** The page table for @p sc, or NULL when out of range. */
const menu_page_t *menu_page(int sc);

/** The hub's sections in tile order; @p n receives the count. */
const uint8_t *menu_hub(int *n);

/* Session sheet actions (MS_MAIN). The sheet is drawn by app_menu.c. */
void menu_act_reset_terminal(uint64_t now);
void menu_act_disconnect(uint64_t now);

/* app_menu.c internals the tables call back into. Shell-wide callers use
 * menu_goto and menu_note (app_screens.h); these stay menu-private. */
void menu_back(uint64_t now);
void menu_present_now(uint64_t now);   /* paint and present before a reboot */
