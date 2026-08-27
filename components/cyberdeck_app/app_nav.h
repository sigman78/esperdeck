/*
 * app_nav.h — screen table + navigation stack (internal to the shell).
 *
 * Screens are hook tables in one compile-time, id-indexed table
 * (SCREENS[], cyberdeck_app.c); navigation is a small
 * stack of (screen, arg) entries, so "back" is nav_pop instead of a
 * bespoke destination per screen. The shell owns the frame:
 * clear → render(now) → [chrome, item 4] → present, once per tick and
 * only when something invalidated — screens never clear or present.
 * (docs/extensibility.md item 2; the design half is docs/ui-spec.md.)
 */

#pragma once

#include "cyberdeck_app.h"
#include "cyberdeck_ui.h"   /* ui_key_t — decoded UI keys */
#include <stdint.h>

/* Shared chrome policy (composited by the shell once item 4's kit lands;
 * recorded per screen now so the descriptor is stable). */
enum { NAV_CHROME_NONE = 0, NAV_CHROME_FULL };

typedef struct {
    const char *name;                    /* debug/logging id            */
    /* Becoming current via push/replace/reset. @p arg is the intent:
     * profile index, import mode, unlock flavor... 0 when unused. */
    void (*enter)(intptr_t arg, uint64_t now);
    /* Revealed again by nav_pop; NULL = use enter with the entry's arg. */
    void (*resume)(intptr_t arg, uint64_t now);
    void (*exit)(uint64_t now);          /* leaving the top; optional   */
    void (*tick)(uint64_t now);
    void (*input)(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                  uint64_t now);
    /* Draw the body into the cleared overlay. NULL = self-managed
     * (SESSION: overlay hidden except its transient chrome pass). */
    void (*render)(uint64_t now);
    uint8_t chrome;                      /* NAV_CHROME_*                */
} nav_screen_t;

/* Adopt the screen table (ids index into it) and reset the stack. Call
 * once at init, before any nav op. False = NULL/empty table or an entry
 * with no descriptor or name. */
bool nav_init(const nav_screen_t *const *screens, int count);

/* Stack ops. Every op exits the departing screen, enters/resumes the
 * arriving one, and invalidates. The bottom entry never pops. False means
 * invalid id, stack full, a push of a screen already on the stack
 * (per-screen state is single-instance), or a pop at the bottom. */
bool nav_push(int id, intptr_t arg, uint64_t now);
bool nav_replace(int id, intptr_t arg, uint64_t now);  /* swap the top  */
bool nav_reset(int id, intptr_t arg, uint64_t now);    /* whole stack   */
bool nav_pop(uint64_t now);

int  nav_current(void);                  /* id of the top screen        */

#ifdef BUILD_SIMULATOR
/* Simulator regression hook: stable descriptor name of the top screen. */
const char *nav_current_name(void);
#endif

/* Request a render this tick (coalesced: one present per frame). */
void nav_invalidate(void);

/* Core loop hooks (cyberdeck_app.c): tick the top screen then run the
 * clear → render → present pass if invalidated; feed one input event. */
void nav_frame(uint64_t now);
void nav_dispatch_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                        uint64_t now);
