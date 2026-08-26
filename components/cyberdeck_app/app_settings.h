/*
 * app_settings.h — the shell's settings tables (internal): one kv table
 * per settings.ini section, schema and defaults live with the owner.
 * main/main.c reads [font] with its own one-field table at boot — keep
 * the "size" key in step.
 */

#pragma once

#include "display_fx.h"
#include "storage_kv.h"
#include <stdbool.h>
#include <stdint.h>

#define APP_SETTINGS_INI   "settings.ini"
#define APP_FX_SECTION     "fx"
#define APP_SAVER_SECTION  "saver"
#define APP_TOUCH_SECTION  "touch"
#define APP_FONT_SECTION   "font"

/* [fx] — display_fx_cfg_t; pre-fill with display_fx_defaults(), the
 * apply path range-clamps, so the table carries no ranges. */
extern const storage_kv_field_t app_fx_fields[];

/* [saver] — idle minutes; doubles as the auto-lock interval. */
#define APP_SAVER_DEFAULT_MIN 3u
typedef struct { uint32_t idle_min; } app_saver_cfg_t;
extern const storage_kv_field_t app_saver_fields[];

/* [touch] — gesture toggles. */
typedef struct { bool scroll; } app_touch_cfg_t;
extern const storage_kv_field_t app_touch_fields[];

/* [font] — staged size name ("8x16"/...), applied at next boot. */
typedef struct { char size[16]; } app_font_cfg_t;
extern const storage_kv_field_t app_font_fields[];

/* Register settings.ini for factory reset; call once at init. */
void app_settings_register_reset(void);
