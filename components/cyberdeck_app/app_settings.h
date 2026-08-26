/*
 * app_settings.h — the shell's settings tables (internal): one kv table
 * per settings.ini section, schema and defaults live with the owner.
 * The file name and the [font] contract are public (cyberdeck_app.h) —
 * main reads [font] at boot. All names defined once, in app_settings.c.
 */

#pragma once

#include "cyberdeck_app.h"   /* cyberdeck_settings_ini, the font contract */
#include "display_fx.h"
#include "storage_kv.h"
#include <stdbool.h>
#include <stdint.h>

/* [fx] — display_fx_cfg_t; pre-fill with display_fx_defaults(), the
 * apply path range-clamps, so the table carries no ranges. */
extern const char app_fx_section[];
extern const storage_kv_field_t app_fx_fields[];

/* [saver] — idle minutes; doubles as the auto-lock interval. */
#define APP_SAVER_DEFAULT_MIN 3u
typedef struct { uint32_t idle_min; } app_saver_cfg_t;
extern const char app_saver_section[];
extern const storage_kv_field_t app_saver_fields[];

/* [touch] — gesture toggles. */
typedef struct { bool scroll; } app_touch_cfg_t;
extern const char app_touch_section[];
extern const storage_kv_field_t app_touch_fields[];

/* Register settings.ini for factory reset; call once at init. */
void app_settings_register_reset(void);
