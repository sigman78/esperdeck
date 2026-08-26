/*
 * app_settings.h — the shell's settings tables (internal to cyberdeck_app).
 *
 * One storage_kv_field_t table per settings file; the schema AND the
 * defaults live here, with the owner — storage only knows key=value
 * (storage_kv.h). The menu's cycling/persistence model still lives in
 * app_menu.c; extensibility plan item 3 splits it out on top of these.
 *
 * font.ini is the one schema shared outside the shell: main/main.c reads
 * it at boot with its own one-field table (see boot_font_size) — keep the
 * "size" key in step.
 */

#pragma once

#include "display_fx.h"
#include "storage_kv.h"
#include <stdbool.h>
#include <stdint.h>

/* fx.ini — display_fx_cfg_t. Pre-fill with display_fx_defaults();
 * display_fx_set() range-clamps on apply, so the table carries no ranges. */
#define APP_FX_INI "fx.ini"
extern const storage_kv_field_t app_fx_fields[];

/* saver.ini — idle minutes; doubles as the auto-lock interval. */
#define APP_SAVER_INI "saver.ini"
#define APP_SAVER_DEFAULT_MIN 3u
typedef struct { uint32_t idle_min; } app_saver_cfg_t;
extern const storage_kv_field_t app_saver_fields[];

/* touch.ini — gesture toggles. */
#define APP_TOUCH_INI "touch.ini"
typedef struct { bool scroll; } app_touch_cfg_t;
extern const storage_kv_field_t app_touch_fields[];

/* font.ini — staged size name ("8x16"/"10x20"/"12x24"), applied at boot. */
#define APP_FONT_INI "font.ini"
typedef struct { char size[16]; } app_font_cfg_t;
extern const storage_kv_field_t app_font_fields[];

/** Register the shell's settings files for factory reset. Call once at
 *  init (after storage_init). */
void app_settings_register_reset(void);
