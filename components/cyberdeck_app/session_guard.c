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

/* Both policies run on HOME only. A live session holds the deck open
 * (v1 semantics), and the gate pad is already locked. */
void session_guard_tick(uint64_t now)
{
    if (nav_current() != SCR_HOME) return;

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
     * walked away from. The vault goes cold along with the hydrated
     * copies out in app state. No-op when locked or without a store. */
    if (!s_guard.engaged && session_guard_idle(now)) {
        s_guard.engaged = true;
        keystore_lock();
        app_creds_wipe();
    }
}
