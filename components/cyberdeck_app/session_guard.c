/*
 * session_guard.c — idle and security policy for the deck as a whole:
 * the idle timer, the idle auto-lock, and the walk-away lock. Policy
 * only. The saver plugin paints over the idle deck. The deck locks
 * the same with or without it (extensibility item 7: security leaves
 * the eye-candy).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_settings.h"
#include "session_guard.h"
#include "keystore.h"

/* Phone unseen this long (after a sighting) locks the deck. */
#define WALKAWAY_MS  60000

static struct {
    uint64_t last_input;
    uint32_t idle_ms;               /* [saver] timeout (= auto-lock)     */
    bool     engaged;               /* idle lock fired for this idle run */
    bool     walkaway_armed;        /* saw the phone since boot          */
} s_guard;

void session_guard_init(uint64_t now)
{
    app_saver_cfg_t sv = { .idle_min = APP_SAVER_DEFAULT_MIN };
    storage_kv_load(cyberdeck_settings_ini, app_saver_section,
                    app_saver_fields, &sv);
    s_guard.idle_ms = sv.idle_min * 60u * 1000u;
    session_guard_activity(now);
}

uint32_t session_guard_idle_min(void)         { return s_guard.idle_ms / 60000u; }
void session_guard_set_idle_min(uint32_t min) { s_guard.idle_ms = min * 60000u; }

void session_guard_activity(uint64_t now)
{
    s_guard.last_input = now;
    s_guard.engaged    = false;
}

bool session_guard_idle(uint64_t now)
{
    return now - s_guard.last_input > s_guard.idle_ms;
}

/* The vault goes cold along with the hydrated copies out in app state.
 * No-op when locked or without a store: without a store, the copies are
 * the only passwords the deck has. */
void session_guard_idle_lock(void)
{
    s_guard.engaged = true;
    if (keystore_state() != KEYSTORE_UNLOCKED) return;
    keystore_lock();
    app_creds_wipe();
}

bool session_guard_lock_gate(uint64_t now)
{
    if (keystore_state() != KEYSTORE_UNLOCKED) return false;
    session_guard_idle_lock();
    unlock_open_gate(now);
    return true;
}

/* Pages the idle lock leaves alone. The pad keeps its own idle rules.
 * The saver only covers HOME or the gate. The connecting page must stop
 * its attempt before it locks, so it calls the lock itself. */
static bool idle_exempt(int scr)
{
    return scr == SCR_UNLOCK || scr == SCR_SAVER || scr == SCR_BOOT ||
           scr == SCR_POWEROFF || scr == SCR_CONNECTING;
}

/* A live session holds the deck open (v1 semantics). That is the session
 * screen or a page above it, such as its menu. The controller state does
 * not tell: a menu Disconnect leaves it at UP. Every other page locks when idle. HOME
 * stays up under the saver. The others give way to the gate pad. The
 * walk-away lock runs on HOME only. */
void session_guard_tick(uint64_t now)
{
    if (nav_on_stack(SCR_SESSION)) return;
    const int scr = nav_current();
    if (scr != SCR_HOME) {
        if (!s_guard.engaged && !idle_exempt(scr) && session_guard_idle(now))
            session_guard_lock_gate(now);
        return;
    }

    /* Walk-away auto-lock (prototype): the phone was HERE, then unseen
     * for a minute, while the store sits unlocked — raise the gate,
     * same path as the L panic key. Armed on a sighting so a deck
     * booted with the phone already absent never self-locks. */
    if (app.presence && keystore_state() == KEYSTORE_UNLOCKED) {
        const cyberdeck_presence_ops_t *pr = app.presence;
        if (pr->present()) {
            s_guard.walkaway_armed = true;
        } else if (s_guard.walkaway_armed && pr->enrolled() &&
                   pr->age_ms() > WALKAWAY_MS) {
            s_guard.walkaway_armed = false;
            keystore_lock();
            app_creds_wipe();
            unlock_open_gate(now);
            return;
        }
    }

    /* Idle auto-lock: a deck idle past the timeout is a deck someone
     * walked away from. */
    if (!s_guard.engaged && session_guard_idle(now))
        session_guard_idle_lock();
}
