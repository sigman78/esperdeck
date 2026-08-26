/*
 * storage_cred.h — the shared credential scratch. Opt-in on purpose: a
 * sharp staging contract for the few hydrate/migrate call sites, kept out
 * of storage.h.
 */

#ifndef STORAGE_CRED_H
#define STORAGE_CRED_H

#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One internal-SRAM buffer for every transient credential staging job.
 * Users run strictly serially on the app task and finish before returning;
 * wipe after use (keystore_wipe). The storage_save_* diversion layer keeps
 * its OWN scratch — it is called with this one as its source. */
typedef struct {
    union {
        conn_profile_t profiles[STORAGE_MAX_PROFILES];  /* ~1.9 KB */
        wifi_profile_t nets[STORAGE_WIFI_MAX];          /* ~0.8 KB */
    } u;
    wifi_profile_t one;        /* single-credential staging */
} storage_cred_scratch_t;

storage_cred_scratch_t *storage_cred_scratch(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_CRED_H */
