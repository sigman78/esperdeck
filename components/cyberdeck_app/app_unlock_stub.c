/*
 * app_unlock_stub.c — inert stand-in for the PIN pad screen (app_unlock.c).
 *
 * Compiled when the secure store is disabled or its sources are absent.
 * With keystore_state() stubbed to ABSENT, no gate ever fires and nothing
 * routes to ST_UNLOCK — these exist only so the screen table and the few
 * explicit call sites link. The KEYSTORE menu entry is dimmed separately
 * (menu_item_dim).
 */

#include "app_internal.h"
#include "app_screens.h"

void unlock_open(uint64_t now, bool resume_connect)
{
    (void)now; (void)resume_connect;
}

void unlock_open_setpin(uint64_t now) { (void)now; }
void unlock_open_remove(uint64_t now) { (void)now; }
void unlock_open_gate(uint64_t now)   { (void)now; }

/* saver_tick_gate stays in app_saver.c — it is plain screensaver
 * machinery with no vault dependency. */

void unlock_tick(uint64_t now) { (void)now; }

void unlock_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                  uint64_t now)
{
    (void)ev; (void)k; (void)ch; (void)now;
}
