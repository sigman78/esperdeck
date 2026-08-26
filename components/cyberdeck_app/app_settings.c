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

/* Frames (~39/s) to a compact "0.5s" string. */
static void fx_secs(char *buf, size_t sz, unsigned frames)
{
    unsigned ms = frames * 26u;
    snprintf(buf, sz, "%u.%us", ms / 1000u, (ms % 1000u) / 100u);
}

void app_settings_fx_format(int t, char *buf, size_t sz)
{
    display_fx_cfg_t c;
    display_fx_get(&c);
    switch (t) {
    case APP_FX_SCAN:
        snprintf(buf, sz, "%s", c.scanlines ? "on" : "off");
        break;
    case APP_FX_MONO:
        snprintf(buf, sz, "%s",
                 c.mono == 0 ? "color" : c.mono == 1 ? "green" : "amber");
        break;
    case APP_FX_BOLD:
        snprintf(buf, sz, "%s", c.bold_pop ? "on" : "off");
        break;
    case APP_FX_WOBBLE:
        snprintf(buf, sz, "%s",
                 c.wobble == 0 ? "off"    : c.wobble == 1 ? "subtle"
               : c.wobble == 2 ? "medium" : "deep");
        break;
#if DISPLAY_FX_ROW_GLOW
    case APP_FX_GLOW:
        snprintf(buf, sz, "%s",
                 !c.glow ? "off" : c.glow_strength ? "strong" : "subtle");
        break;
    case APP_FX_DECAY:
        fx_secs(buf, sz, c.glow_frames);
        break;
#endif
    case APP_FX_WIPE:
        if (!c.wipe) snprintf(buf, sz, "off");
        else         fx_secs(buf, sz, c.wipe_frames);
        break;
    case APP_FX_COLLAPSE:
        if (!c.collapse) snprintf(buf, sz, "off");
        else             fx_secs(buf, sz, c.collapse_frames);
        break;
    case APP_FX_STATIC:
        snprintf(buf, sz, "%s",
                 !c.static_burst       ? "off"
                 : c.static_lines <= 1 ? "light"
                 : c.static_lines == 2 ? "medium"
                                       : "heavy");
        break;
    default: buf[0] = '\0'; break;
    }
}

/* Step the tunable, apply live, and (for the event effects) arm a one-shot
 * preview so the change is seen immediately. The [fx] write is deferred. */
void app_settings_fx_cycle(int t)
{
    display_fx_cfg_t c;
    display_fx_get(&c);
    switch (t) {
    case APP_FX_SCAN:   c.scanlines = !c.scanlines;               break;
    case APP_FX_MONO:   c.mono = (uint8_t)((c.mono + 1) % 3);     break;
    case APP_FX_BOLD:   c.bold_pop = !c.bold_pop;                 break;
    case APP_FX_WOBBLE: c.wobble = (uint8_t)((c.wobble + 1) % 4); break;
#if DISPLAY_FX_ROW_GLOW
    case APP_FX_GLOW:   /* off -> subtle -> strong -> off */
        if (!c.glow)               { c.glow = 1; c.glow_strength = 0; }
        else if (!c.glow_strength)   c.glow_strength = 1;
        else                         c.glow = 0;
        break;
    case APP_FX_DECAY:  /* 0.3s -> 0.5s -> 1s -> 2s -> 0.3s */
        c.glow_frames = c.glow_frames < 16 ? 20
                      : c.glow_frames < 30 ? 39
                      : c.glow_frames < 60 ? 78 : 12;
        break;
#endif
    case APP_FX_WIPE:   /* off -> fast -> slow -> off */
        if (!c.wipe)                { c.wipe = 1; c.wipe_frames = 12; }
        else if (c.wipe_frames <= 12) c.wipe_frames = 24;
        else                          c.wipe = 0;
        break;
    case APP_FX_COLLAPSE:
        if (!c.collapse)                { c.collapse = 1; c.collapse_frames = 8; }
        else if (c.collapse_frames <= 8)  c.collapse_frames = 20;
        else                              c.collapse = 0;
        break;
    case APP_FX_STATIC: /* off -> light -> heavy -> off */
        if (!c.static_burst) { c.static_burst = 1; c.static_frames = 8;  c.static_lines = 1; }
        else if (c.static_lines <= 1) { c.static_frames = 14; c.static_lines = 3; }
        else c.static_burst = 0;
        break;
    default: return;
    }
    display_fx_set(&c);
    s_dirty |= DIRTY_FX;
    if (t == APP_FX_WIPE && c.wipe)           display_fx_wipe();
    if (t == APP_FX_COLLAPSE && c.collapse)   display_fx_collapse();
    if (t == APP_FX_STATIC && c.static_burst) display_fx_static();
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
