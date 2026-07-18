/*
 * app_menu_defs.c — the static menu tree: page titles, items, colors, and
 * the predicates (confirm / dim) the renderer and activator share.
 * See app_menu_defs.h for the page enum; app_menu.c for behavior.
 */

#include "app_menu_defs.h"
#include "app_internal.h"

/* Menu color law — one color per KIND of item, not per item (a page of
 * many-colored bars reads as motley):
 *   CYAN  = normal action / navigation      GREEN = go / primary
 *   RED   = destructive                     AMBER = caution
 *   BLUE  = Back / Cancel (safe exit)
 * MAGENTA stays reserved for the title lozenge. */
static const char *main_items[]     = { "Resume session", "Disconnect", "Configuration >" };
static const uint8_t main_cols[]    = { OVERLAY_COL_GREEN, OVERLAY_COL_AMBER, OVERLAY_COL_CYAN };

static const char *config_items[]   = { "Profiles >", "WiFi >", "Keyboard >", "Effects >",
                                        "System >", "Back" };
static const uint8_t config_cols[]  = { OVERLAY_COL_CYAN, OVERLAY_COL_CYAN,
                                        OVERLAY_COL_CYAN, OVERLAY_COL_CYAN,
                                        OVERLAY_COL_CYAN, OVERLAY_COL_BLUE };

static const char *profiles_items[] = { "Add (type here)", "Edit", "Reorder",
                                        "Delete", "Import >", "Back" };
static const uint8_t profiles_cols[]= { OVERLAY_COL_CYAN, OVERLAY_COL_CYAN,
                                        OVERLAY_COL_CYAN, OVERLAY_COL_RED,
                                        OVERLAY_COL_CYAN, OVERLAY_COL_BLUE };

static const char *import_items[]   = { "SoftAP (phone)", "Web (PC)", "Back" };
static const uint8_t import_cols[]  = { OVERLAY_COL_CYAN, OVERLAY_COL_CYAN, OVERLAY_COL_BLUE };

static const char *wifi_items[]     = { "Reconnect", "Add network (phone)", "Back" };
static const uint8_t wifi_cols[]    = { OVERLAY_COL_CYAN, OVERLAY_COL_CYAN, OVERLAY_COL_BLUE };

static const char *kbd_items[]      = { "Pair keyboard", "Forget bonds", "Back" };
static const uint8_t kbd_cols[]     = { OVERLAY_COL_CYAN, OVERLAY_COL_RED, OVERLAY_COL_BLUE };

static const char *system_items[]   = { "Clear host keys", "Factory reset", "Back" };
static const uint8_t system_cols[]  = { OVERLAY_COL_RED, OVERLAY_COL_RED, OVERLAY_COL_BLUE };

/* Static definition for a screen; the profile pickers are built dynamically. */
menu_def_t menu_def(int sc)
{
    switch (sc) {
    case MS_MAIN:     return (menu_def_t){ "MENU",          main_items,     main_cols,     NELEM(main_items) };
    case MS_CONFIG:   return (menu_def_t){ "CONFIGURATION", config_items,   config_cols,   NELEM(config_items) };
    case MS_PROFILES: return (menu_def_t){ "PROFILES",      profiles_items, profiles_cols, NELEM(profiles_items) };
    case MS_IMPORT:   return (menu_def_t){ "IMPORT",        import_items,   import_cols,   NELEM(import_items) };
    case MS_WIFI:     return (menu_def_t){ "WIFI",          wifi_items,     wifi_cols,     NELEM(wifi_items) };
    case MS_KEYBOARD: return (menu_def_t){ "KEYBOARD",      kbd_items,      kbd_cols,      NELEM(kbd_items) };
    case MS_SYSTEM:   return (menu_def_t){ "SYSTEM",        system_items,   system_cols,   NELEM(system_items) };
    default:          return (menu_def_t){ "", NULL, NULL, 0 };
    }
}

/* True for the dynamically built stored-profile pickers. */
bool menu_is_picker(int sc)
{
    return sc == MS_DELPROFILE || sc == MS_EDITPROFILE || sc == MS_REORDER;
}

/* CONFIRM label if (sc,sel) is a destructive 2-step action, else NULL. In the
 * delete picker every profile tile (sel < stored_count) is destructive. */
const char *menu_confirm(int sc, int sel)
{
    if (sc == MS_KEYBOARD   && sel == 1) return "CONFIRM forget bonds?";
    if (sc == MS_SYSTEM     && sel == 0) return "CONFIRM clear host keys?";
    if (sc == MS_SYSTEM     && sel == 1) return "CONFIRM FACTORY RESET?";
    if (sc == MS_DELPROFILE && sel < app.stored_count) return "CONFIRM delete?";
    return NULL;
}

/* Is (sc,sel) unavailable because BLE support is absent? */
bool menu_item_dim(int sc, int sel)
{
    if (app.cfg.ble) return false;
    return (sc == MS_CONFIG && sel == CFG_KEYBOARD) || (sc == MS_KEYBOARD);
}
