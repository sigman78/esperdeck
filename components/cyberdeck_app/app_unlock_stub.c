/*
 * app_unlock_stub.c — inert stand-in for the PIN pad screen (app_unlock.c).
 *
 * The build includes this file when Kconfig disables the secure store,
 * or its sources are absent.
 * With keystore_state() stubbed to ABSENT, no gate ever fires and nothing
 * navigates to this screen. The descriptor and semantic entry points exist
 * only so both feature configurations expose the same shell linkage surface.
 * menu_item_dim dims the KEYSTORE menu entry separately.
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
bool unlock_is_gate(void)             { return false; }

const nav_screen_t unlock_screen = {
    .name = "unlock-disabled",
    .chrome = NAV_CHROME_FULL,
};
