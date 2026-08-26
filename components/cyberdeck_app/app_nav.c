/*
 * app_nav.c — screen registry, navigation stack, and the shell-owned
 * frame pass. See app_nav.h.
 */

#include "app_nav.h"
#include "app_internal.h"

#include "esp_log.h"
#include <string.h>

static const char *TAG = "nav";

#define NAV_SCREENS_MAX 32
#define NAV_STACK_MAX    6

static const nav_screen_t *s_screens[NAV_SCREENS_MAX];
static int s_screen_count;

typedef struct { int id; intptr_t arg; } nav_entry_t;
static nav_entry_t s_stack[NAV_STACK_MAX];
static int  s_depth;       /* 0 = nothing entered yet (init only) */
static bool s_dirty;

void nav_init(void)
{
    for (int i = 0; i < NAV_SCREENS_MAX; i++) s_screens[i] = NULL;
    memset(s_stack, 0, sizeof(s_stack));
    s_screen_count = 0;
    s_depth        = 0;
    s_dirty        = false;
}

int nav_register(const nav_screen_t *def)
{
    if (!def || !def->name) {
        ESP_LOGE(TAG, "invalid screen descriptor");
        return -1;
    }
    if (s_screen_count >= NAV_SCREENS_MAX) {
        ESP_LOGE(TAG, "screen registry full (%d), rejected %s",
                 NAV_SCREENS_MAX, def->name);
        return -1;
    }
    s_screens[s_screen_count] = def;
    return s_screen_count++;
}

static const nav_screen_t *screen(int id)
{
    return (id >= 0 && id < s_screen_count) ? s_screens[id] : NULL;
}

int nav_current(void)
{
    return s_depth > 0 ? s_stack[s_depth - 1].id : -1;
}

#ifdef BUILD_SIMULATOR
const char *nav_current_name(void)
{
    const nav_screen_t *d = screen(nav_current());
    return d && d->name ? d->name : "";
}
#endif

void nav_invalidate(void)
{
    s_dirty = true;
}

static void leave_top(uint64_t now)
{
    const nav_screen_t *d = s_depth > 0 ? screen(nav_current()) : NULL;
    if (d && d->exit) d->exit(now);
}

static void enter_top(bool via_pop, uint64_t now)
{
    const nav_entry_t  *e = &s_stack[s_depth - 1];
    const nav_screen_t *d = screen(e->id);
    if (!d) return;
    if (via_pop && d->resume) d->resume(e->arg, now);
    else if (d->enter)        d->enter(e->arg, now);
    s_dirty = true;
}

bool nav_push(int id, intptr_t arg, uint64_t now)
{
    if (!screen(id)) {
        ESP_LOGE(TAG, "push of invalid screen id %d", id);
        return false;
    }
    if (s_depth >= NAV_STACK_MAX) {
        ESP_LOGW(TAG, "stack full pushing %s", screen(id)->name);
        return false;
    }
    leave_top(now);
    s_stack[s_depth++] = (nav_entry_t){ id, arg };
    enter_top(false, now);
    return true;
}

bool nav_replace(int id, intptr_t arg, uint64_t now)
{
    if (!screen(id)) {
        ESP_LOGE(TAG, "replace with invalid screen id %d", id);
        return false;
    }
    if (s_depth == 0) return nav_push(id, arg, now);
    leave_top(now);
    s_stack[s_depth - 1] = (nav_entry_t){ id, arg };
    enter_top(false, now);
    return true;
}

bool nav_reset(int id, intptr_t arg, uint64_t now)
{
    if (!screen(id)) {
        ESP_LOGE(TAG, "reset to invalid screen id %d", id);
        return false;
    }
    leave_top(now);
    s_depth    = 1;
    s_stack[0] = (nav_entry_t){ id, arg };
    enter_top(false, now);
    return true;
}

bool nav_pop(uint64_t now)
{
    if (s_depth <= 1) return false;       /* the bottom entry never pops */
    leave_top(now);
    s_depth--;
    enter_top(true, now);
    return true;
}

void nav_frame(uint64_t now)
{
    const nav_screen_t *d = screen(nav_current());
    if (!d) return;
    if (d->tick) d->tick(now);

    /* A tick may have navigated — render whoever is on top now. */
    if (!s_dirty) return;
    s_dirty = false;
    d = screen(nav_current());
    if (!d || !d->render) return;        /* self-managed (session) */

    ui_colors(UI_FG, UI_BG);
    ui_clear();
    d->render(now);
    /* Shared chrome composites here once item 4's kit lands (d->chrome). */
    ui_no_cursor();
    ui_present();
}

void nav_dispatch_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                        uint64_t now)
{
    const nav_screen_t *d = screen(nav_current());
    if (d && d->input) d->input(ev, k, ch, now);
}
