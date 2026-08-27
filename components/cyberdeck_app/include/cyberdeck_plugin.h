/*
 * cyberdeck_plugin.h — the plugin seam (extensibility item 5).
 *
 * A plugin is a statically linked module (the scope decision: an
 * interfacing discipline, never dynamic loading). Its screens and menu
 * pages are rows in the shell's compile-time tables; everything else it
 * contributes rides this descriptor, listed ONCE in plugin_table.c —
 * the single edit point per plugin, identical on device and simulator.
 * Every field is optional: a plugin can be just a tick, just a HOME
 * tile, or a full feature with settings.
 *
 * Optional platform capabilities (BLE keyboard, phone presence) are
 * resolved by name with cyberdeck_service() — absent on platforms that
 * did not register them, so always handle NULL.
 */

#pragma once

#include "cyberdeck_ui.h"
#include "esp_err.h"
#include "storage_kv.h"
#include <stdint.h>

/* One trailing HOME tile (rendered after the profiles, before the
 * Configuration tile). Shown while visible() (NULL = always). */
typedef struct {
    const char *label;
    const char *body;
    uint8_t     accent;                  /* OVERLAY_COL_*              */
    bool      (*visible)(void);
    void      (*activate)(uint64_t now);
} home_tile_t;

typedef struct {
    const char *name;      /* stable id: logs and the settings section */

    /* Called once after core services are up, before the first frame.
     * A failure is logged, not fatal — the deck boots without you. */
    esp_err_t (*init)(void);

    /* Polled from the shell tick (~every 50 ms); gate internally for
     * slower cadences. No event bus — by design. */
    void (*tick)(uint64_t now);

    /* Deferred flash writes: called every tick, expected to no-op
     * unless something is dirty and no screen holds the write back
     * (the app_settings_idle_flush pattern). */
    void (*idle_flush)(void);

    /* Persistent settings: a NULL-terminated kv table filling
     * @p settings_obj, loaded before init() from the plugin's own
     * [name] section of settings.ini (which is already registered
     * for factory reset). Persisting is the plugin's job — its
     * idle_flush typically saves the same triple. */
    const storage_kv_field_t *settings;
    void *settings_obj;

    /* Trailing HOME tiles. */
    const home_tile_t *home_tiles;
    int n_home_tiles;
} cyberdeck_plugin_t;

/* plugin_table.c — the built plugins, in HOME-tile display order. */
extern const cyberdeck_plugin_t *const cyberdeck_plugins[];
extern const int cyberdeck_plugin_count;
