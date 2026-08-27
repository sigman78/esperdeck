/*
 * app_settings.c — the shell's settings tables + the settings model
 * (value cycling, deferred persistence). See app_settings.h.
 */

#include "app_settings.h"
#include "app_screens.h"     /* saver_idle_min/saver_set_idle_min */

#include <stddef.h>
#include <stdio.h>

/* The one definition of every settings name. */
const char cyberdeck_settings_ini[] = "settings.ini";
const char cyberdeck_font_section[] = "font";
const char app_fx_section[]         = "fx";
const char app_saver_section[]      = "saver";
const char app_touch_section[]      = "touch";

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

const storage_kv_field_t cyberdeck_font_fields[] = {
    { "size", offsetof(cyberdeck_font_cfg_t, size), STORAGE_KV_STR,
      sizeof(((cyberdeck_font_cfg_t *)0)->size), 0, 0 },
    { NULL, 0, 0, 0, 0, 0 },
};

void app_settings_register_reset(void)
{
    storage_reset_register(cyberdeck_settings_ini);
}

/* ------------------------------------------------------ settings model */

enum { DIRTY_FX = 1 << 0, DIRTY_SAVER = 1 << 1, DIRTY_TOUCH = 1 << 2 };
static uint8_t s_dirty, s_hold;

/* fx presets — one ordered table per tunable: labels for the UI, index()
 * snapping the live cfg to a preset, apply() writing the (possibly
 * composite) fields. Times are label'd from the fixed preset frames. */

typedef struct {
    const char *const *labels;
    uint8_t count;
    int  (*index)(const display_fx_cfg_t *c);
    void (*apply)(display_fx_cfg_t *c, int idx);
} fx_preset_ops_t;

static const char *const ONOFF_L[] = { "off", "on" };
static const char *const MONO_L[]  = { "color", "green", "amber" };
static const char *const WOB_L[]   = { "off", "subtle", "medium", "deep" };
#if DISPLAY_FX_ROW_GLOW
static const char *const GLOW_L[]  = { "off", "subtle", "strong" };
static const char *const DECAY_L[] = { "0.3s", "0.5s", "1.0s", "2.0s" };
static const uint8_t     DECAY_F[] = { 12, 20, 39, 78 };
#endif
static const char *const WIPE_L[]  = { "off", "0.3s", "0.6s" };
static const char *const COLL_L[]  = { "off", "0.2s", "0.5s" };
static const char *const STAT_L[]  = { "off", "light", "heavy" };

static int  ix_scan(const display_fx_cfg_t *c) { return c->scanlines ? 1 : 0; }
static void ap_scan(display_fx_cfg_t *c, int i) { c->scanlines = (uint8_t)i; }
static int  ix_mono(const display_fx_cfg_t *c) { return c->mono < 3 ? c->mono : 0; }
static void ap_mono(display_fx_cfg_t *c, int i) { c->mono = (uint8_t)i; }
static int  ix_bold(const display_fx_cfg_t *c) { return c->bold_pop ? 1 : 0; }
static void ap_bold(display_fx_cfg_t *c, int i) { c->bold_pop = (uint8_t)i; }
static int  ix_wob(const display_fx_cfg_t *c) { return c->wobble < 4 ? c->wobble : 0; }
static void ap_wob(display_fx_cfg_t *c, int i) { c->wobble = (uint8_t)i; }

#if DISPLAY_FX_ROW_GLOW
static int ix_glow(const display_fx_cfg_t *c)
{
    return !c->glow ? 0 : c->glow_strength ? 2 : 1;
}
static void ap_glow(display_fx_cfg_t *c, int i)
{
    c->glow = (uint8_t)(i > 0);
    if (i) c->glow_strength = (uint8_t)(i == 2);
}
static int ix_decay(const display_fx_cfg_t *c)
{
    return c->glow_frames < 16 ? 0 : c->glow_frames < 30 ? 1
         : c->glow_frames < 60 ? 2 : 3;
}
static void ap_decay(display_fx_cfg_t *c, int i)
{
    c->glow_frames = DECAY_F[i];
}
#endif

static int ix_wipe(const display_fx_cfg_t *c)
{
    return !c->wipe ? 0 : c->wipe_frames <= 12 ? 1 : 2;
}
static void ap_wipe(display_fx_cfg_t *c, int i)
{
    c->wipe = (uint8_t)(i > 0);
    if (i) c->wipe_frames = (uint8_t)(i == 2 ? 24 : 12);
}
static int ix_coll(const display_fx_cfg_t *c)
{
    return !c->collapse ? 0 : c->collapse_frames <= 8 ? 1 : 2;
}
static void ap_coll(display_fx_cfg_t *c, int i)
{
    c->collapse = (uint8_t)(i > 0);
    if (i) c->collapse_frames = (uint8_t)(i == 2 ? 20 : 8);
}
static int ix_stat(const display_fx_cfg_t *c)
{
    return !c->static_burst ? 0 : c->static_lines <= 1 ? 1 : 2;
}
static void ap_stat(display_fx_cfg_t *c, int i)
{
    c->static_burst = (uint8_t)(i > 0);
    if (i) {
        c->static_frames = (uint8_t)(i == 2 ? 14 : 8);
        c->static_lines  = (uint8_t)(i == 2 ? 3 : 1);
    }
}

static const fx_preset_ops_t FX_PRESETS[] = {
    [APP_FX_SCAN]     = { ONOFF_L, NELEM(ONOFF_L), ix_scan, ap_scan },
    [APP_FX_MONO]     = { MONO_L,  NELEM(MONO_L),  ix_mono, ap_mono },
    [APP_FX_BOLD]     = { ONOFF_L, NELEM(ONOFF_L), ix_bold, ap_bold },
    [APP_FX_WOBBLE]   = { WOB_L,   NELEM(WOB_L),   ix_wob,  ap_wob  },
#if DISPLAY_FX_ROW_GLOW
    [APP_FX_GLOW]     = { GLOW_L,  NELEM(GLOW_L),  ix_glow, ap_glow },
    [APP_FX_DECAY]    = { DECAY_L, NELEM(DECAY_L), ix_decay, ap_decay },
#endif
    [APP_FX_WIPE]     = { WIPE_L,  NELEM(WIPE_L),  ix_wipe, ap_wipe },
    [APP_FX_COLLAPSE] = { COLL_L,  NELEM(COLL_L),  ix_coll, ap_coll },
    [APP_FX_STATIC]   = { STAT_L,  NELEM(STAT_L),  ix_stat, ap_stat },
};

int app_settings_fx_count(app_fx_tunable_t t)
{
    return (unsigned)t < (unsigned)NELEM(FX_PRESETS) ? FX_PRESETS[t].count : 0;
}

int app_settings_fx_index(app_fx_tunable_t t)
{
    if ((unsigned)t >= (unsigned)NELEM(FX_PRESETS)) return 0;
    display_fx_cfg_t c;
    display_fx_get(&c);
    return FX_PRESETS[t].index(&c);
}

const char *app_settings_fx_label(app_fx_tunable_t t, int idx)
{
    if ((unsigned)t >= (unsigned)NELEM(FX_PRESETS)) return "";
    const fx_preset_ops_t *p = &FX_PRESETS[t];
    return (idx >= 0 && idx < p->count) ? p->labels[idx] : "";
}

/* Apply preset @p idx live; the [fx] write stays deferred. An enabled
 * event effect gets a one-shot preview so the change is seen at once. */
void app_settings_fx_set(app_fx_tunable_t t, int idx)
{
    if ((unsigned)t >= (unsigned)NELEM(FX_PRESETS)) return;
    const fx_preset_ops_t *p = &FX_PRESETS[t];
    if (idx < 0 || idx >= p->count) return;
    display_fx_cfg_t c;
    display_fx_get(&c);
    p->apply(&c, idx);
    display_fx_set(&c);
    s_dirty |= DIRTY_FX;
    if (idx > 0) {
        if (t == APP_FX_WIPE)     display_fx_wipe();
        if (t == APP_FX_COLLAPSE) display_fx_collapse();
        if (t == APP_FX_STATIC)   display_fx_static();
    }
}

void app_settings_fx_cycle(app_fx_tunable_t t)
{
    const int n = app_settings_fx_count(t);
    if (n) app_settings_fx_set(t, (app_settings_fx_index(t) + 1) % n);
}

/* Saver timeout steps (minutes) — the saver engage also wipes the MK
 * (master key), so this doubles as the auto-lock interval. */
static const uint8_t SAVER_STEPS[] = { 1, 3, 5, 10, 30 };

void app_settings_saver_cycle(void)
{
    const uint32_t cur = saver_idle_min();
    int next = 0;                        /* unknown (hand-edited): snap */
    for (int k = 0; k < NELEM(SAVER_STEPS); k++)
        if (SAVER_STEPS[k] == cur) {
            next = (k + 1) % NELEM(SAVER_STEPS);
            break;
        }
    saver_set_idle_min(SAVER_STEPS[next]);
    s_dirty |= DIRTY_SAVER;
}

void app_settings_saver_format(char *buf, size_t sz)
{
    snprintf(buf, sz, "%u min", (unsigned)saver_idle_min());
}

#if CONFIG_INPUT_TOUCH_SCROLL
void app_settings_touch_toggle(void)
{
    app.touch_scroll = !app.touch_scroll;
    app_touch_scroll_apply();
    s_dirty |= DIRTY_TOUCH;
}

const char *app_settings_touch_str(void)
{
    return app.touch_scroll ? "on" : "off";
}
#endif

void app_settings_hold(uint8_t mask)
{
    s_hold = mask;
}

void app_settings_dirty_discard(void)
{
    s_dirty = 0;
}

void app_settings_idle_flush(void)
{
    if ((s_dirty & DIRTY_FX) && !(s_hold & APP_SETTINGS_HOLD_FX)) {
        s_dirty &= (uint8_t)~DIRTY_FX;
        display_fx_cfg_t c;
        display_fx_get(&c);
        storage_kv_save(cyberdeck_settings_ini, app_fx_section,
                        app_fx_fields, &c);
    }
    if ((s_dirty & DIRTY_SAVER) && !(s_hold & APP_SETTINGS_HOLD_SYS)) {
        s_dirty &= (uint8_t)~DIRTY_SAVER;
        app_saver_cfg_t sv = { .idle_min = saver_idle_min() };
        storage_kv_save(cyberdeck_settings_ini, app_saver_section,
                        app_saver_fields, &sv);
    }
    if ((s_dirty & DIRTY_TOUCH) && !(s_hold & APP_SETTINGS_HOLD_SYS)) {
        s_dirty &= (uint8_t)~DIRTY_TOUCH;
        app_touch_cfg_t tc = { .scroll = app.touch_scroll };
        storage_kv_save(cyberdeck_settings_ini, app_touch_section,
                        app_touch_fields, &tc);
    }
}
