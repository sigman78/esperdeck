/*
 * app_settings.h — the shell's settings tables (internal): one kv table
 * per settings.ini section, schema and defaults live with the owner.
 * The file name and the [font] contract are public (cyberdeck_app.h) —
 * main reads [font] at boot. All names defined once, in app_settings.c.
 */

#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"       /* provides CONFIG_INPUT_TOUCH_SCROLL (sim: -D flag) */
#endif

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

/* Value cycling and deferred persistence for the menu's tunable tiles
 * (extensibility item 3). The owning page defers writes while it stays
 * open. A flash write pauses the render ISR for the cache-off window,
 * landing a visible hiccup on the keypress. The app tick flushes the
 * write once the hold lifts, so forced exits still save. */

typedef enum {
    APP_FX_SCAN, APP_FX_MONO, APP_FX_BOLD, APP_FX_WOBBLE,
#if DISPLAY_FX_ROW_GLOW
    APP_FX_GLOW, APP_FX_DECAY,
#endif
    APP_FX_WIPE, APP_FX_COLLAPSE, APP_FX_STATIC,
} app_fx_tunable_t;

/* Each tunable is an ordered table of quantized presets (settings.ini
 * keeps byte-granular control; index() snaps hand-edited values to the
 * nearest preset). set() applies live and previews the event effects;
 * the flash write stays deferred. count/index/set is the Slider/Stepper
 * contract (ui-spec); cycle is the tap-to-step sugar over it. */
int         app_settings_fx_count(app_fx_tunable_t t);
int         app_settings_fx_index(app_fx_tunable_t t);
const char *app_settings_fx_label(app_fx_tunable_t t, int idx);
void        app_settings_fx_set(app_fx_tunable_t t, int idx);
void        app_settings_fx_cycle(app_fx_tunable_t t);

void app_settings_saver_cycle(void);        /* 1/3/5/10/30 min steps */
void app_settings_saver_format(char *buf, size_t sz);

#if CONFIG_INPUT_TOUCH_SCROLL
void        app_settings_touch_toggle(void);
const char *app_settings_touch_str(void);
#endif

enum { APP_SETTINGS_HOLD_FX = 1 << 0, APP_SETTINGS_HOLD_SYS = 1 << 1 };
/* Defer dirty writes for the given groups (the menu sets this while the
 * EFFECTS/SYSTEM page is open; 0 releases everything). */
void app_settings_hold(uint8_t mask);
/* Write whatever is dirty and not held. Call from the app tick. */
void app_settings_idle_flush(void);
/* Forget pending writes (factory reset must not resurrect the file). */
void app_settings_dirty_discard(void);
