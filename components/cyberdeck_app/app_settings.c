/*
 * app_settings.c — the shell's settings tables. See app_settings.h.
 */

#include "app_settings.h"

#include <stddef.h>

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

void app_settings_register_reset(void)
{
    storage_reset_register(APP_SETTINGS_INI);
}
