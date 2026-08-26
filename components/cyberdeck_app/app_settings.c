/*
 * app_settings.c — the shell's settings tables + the legacy-file
 * migration. See app_settings.h.
 */

#include "app_settings.h"

#include "storage.h"     /* storage_platform_mount_point (legacy cleanup) */

#include <stddef.h>
#include <stdio.h>

const storage_kv_field_t app_fx_fields[] = {
    { "scanlines",       offsetof(display_fx_cfg_t, scanlines),       STORAGE_KV_U8, 0, 0, 0 },
    { "bold_pop",        offsetof(display_fx_cfg_t, bold_pop),        STORAGE_KV_U8, 0, 0, 0 },
    { "mono",            offsetof(display_fx_cfg_t, mono),            STORAGE_KV_U8, 0, 0, 0 },
    { "glow",            offsetof(display_fx_cfg_t, glow),            STORAGE_KV_U8, 0, 0, 0 },
    { "glow_frames",     offsetof(display_fx_cfg_t, glow_frames),     STORAGE_KV_U8, 0, 0, 0 },
    { "glow_strength",   offsetof(display_fx_cfg_t, glow_strength),   STORAGE_KV_U8, 0, 0, 0 },
    { "wipe",            offsetof(display_fx_cfg_t, wipe),            STORAGE_KV_U8, 0, 0, 0 },
    { "wipe_frames",     offsetof(display_fx_cfg_t, wipe_frames),     STORAGE_KV_U8, 0, 0, 0 },
    { "collapse",        offsetof(display_fx_cfg_t, collapse),        STORAGE_KV_U8, 0, 0, 0 },
    { "collapse_frames", offsetof(display_fx_cfg_t, collapse_frames), STORAGE_KV_U8, 0, 0, 0 },
    { "static",          offsetof(display_fx_cfg_t, static_burst),    STORAGE_KV_U8, 0, 0, 0 },
    { "static_frames",   offsetof(display_fx_cfg_t, static_frames),   STORAGE_KV_U8, 0, 0, 0 },
    { "static_lines",    offsetof(display_fx_cfg_t, static_lines),    STORAGE_KV_U8, 0, 0, 0 },
    { "wobble",          offsetof(display_fx_cfg_t, wobble),          STORAGE_KV_U8, 0, 0, 0 },
    { NULL, 0, 0, 0, 0, 0 },
};

const storage_kv_field_t app_saver_fields[] = {
    { "idle_min", offsetof(app_saver_cfg_t, idle_min), STORAGE_KV_U32, 0, 1, 60 },
    { NULL, 0, 0, 0, 0, 0 },
};

const storage_kv_field_t app_touch_fields[] = {
    { "scroll", offsetof(app_touch_cfg_t, scroll), STORAGE_KV_BOOL, 0, 0, 0 },
    { NULL, 0, 0, 0, 0, 0 },
};

const storage_kv_field_t app_font_fields[] = {
    { "size", offsetof(app_font_cfg_t, size), STORAGE_KV_STR,
      sizeof(((app_font_cfg_t *)0)->size), 0, 0 },
    { NULL, 0, 0, 0, 0, 0 },
};

/* ---- legacy migration -------------------------------------------------
 * Before 2026-08 every setting had its own flat file (fx/saver/touch/
 * font.ini). Fold whatever still exists into settings.ini sections and
 * delete the originals, once. Loads prefer the section when both exist
 * (a re-run after a partial migration must not resurrect stale values).
 * -------------------------------------------------------------------- */

typedef struct {
    const char *legacy;                 /* old flat file        */
    const char *section;                /* settings.ini section */
    const storage_kv_field_t *fields;
} legacy_map_t;

void app_settings_migrate(void)
{
    display_fx_cfg_t fx;
    app_saver_cfg_t  sv = { .idle_min = APP_SAVER_DEFAULT_MIN };
    app_touch_cfg_t  tc = { .scroll = true };
    app_font_cfg_t   fc = { .size = "" };
    display_fx_defaults(&fx);

    const legacy_map_t map[] = {
        { "fx.ini",    APP_FX_SECTION,    app_fx_fields    },
        { "saver.ini", APP_SAVER_SECTION, app_saver_fields },
        { "touch.ini", APP_TOUCH_SECTION, app_touch_fields },
        { "font.ini",  APP_FONT_SECTION,  app_font_fields  },
    };
    void *const objs[] = { &fx, &sv, &tc, &fc };

    bool any = false;
    for (int i = 0; i < 4; i++) {
        if (storage_kv_load(map[i].legacy, NULL, map[i].fields, objs[i])
            != ESP_OK)
            continue;
        /* Legacy file present. Section already there? Section wins. */
        storage_kv_load(APP_SETTINGS_INI, map[i].section,
                        map[i].fields, objs[i]);
        if (storage_kv_save(APP_SETTINGS_INI, map[i].section,
                            map[i].fields, objs[i]) == ESP_OK) {
            char path[160];
            snprintf(path, sizeof(path), "%s/%s",
                     storage_platform_mount_point(), map[i].legacy);
            remove(path);
            any = true;
        }
    }
    (void)any;
}

void app_settings_register_reset(void)
{
    storage_reset_register(APP_SETTINGS_INI);
    /* Legacy names: a factory reset on a deck that never booted this
     * firmware's migration must still clear them. Drop these entries a
     * release or two after the settings.ini consolidation (2026-08). */
    storage_reset_register("fx.ini");
    storage_reset_register("saver.ini");
    storage_reset_register("touch.ini");
    storage_reset_register("font.ini");
}
