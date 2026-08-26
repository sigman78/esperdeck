/*
 * app_settings.h — the shell's settings tables (internal to cyberdeck_app).
 *
 * One storage_kv_field_t table per settings SECTION of settings.ini; the
 * schema AND the defaults live here, with the owner — storage only knows
 * key=value (storage_kv.h). The menu's cycling/persistence model still
 * lives in app_menu.c; extensibility plan item 3 splits it out on top of
 * these.
 *
 * [font] is the one schema shared outside the shell: main/main.c reads it
 * at boot with its own one-field table (see boot_font_size) — keep the
 * "size" key in step.
 */

#pragma once

#include "display_fx.h"
#include "storage_kv.h"
#include <stdbool.h>
#include <stdint.h>

/* One file, one section per concern. */
#define APP_SETTINGS_INI   "settings.ini"
#define APP_FX_SECTION     "fx"
#define APP_SAVER_SECTION  "saver"
#define APP_TOUCH_SECTION  "touch"
#define APP_FONT_SECTION   "font"

/* [fx] — display_fx_cfg_t. Pre-fill with display_fx_defaults();
 * display_fx_set() range-clamps on apply, so the table carries no ranges. */
extern const storage_kv_field_t app_fx_fields[];

/* [saver] — idle minutes; doubles as the auto-lock interval. */
#define APP_SAVER_DEFAULT_MIN 3u
typedef struct { uint32_t idle_min; } app_saver_cfg_t;
extern const storage_kv_field_t app_saver_fields[];

/* [touch] — gesture toggles. */
typedef struct { bool scroll; } app_touch_cfg_t;
extern const storage_kv_field_t app_touch_fields[];

/* [font] — staged size name ("8x16"/"10x20"/"12x24"), applied at boot. */
typedef struct { char size[16]; } app_font_cfg_t;
extern const storage_kv_field_t app_font_fields[];

/** Register settings.ini for factory reset. Call once at init (after
 *  storage_init). */
void app_settings_register_reset(void);
