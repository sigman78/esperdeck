/*
 * storage_cred.h — the shared credential scratch buffer. Deliberately NOT
 * in storage.h: this is a sharp staging contract for the few call sites
 * that hydrate or migrate credentials, not general API.
 */

#ifndef STORAGE_CRED_H
#define STORAGE_CRED_H

#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One internal-SRAM buffer for every transient profile/PSK staging job
 * (bundle adoption, remove-code restore, NVS migration, fallback seed,
 * the WiFi kick). All users run strictly serially on the app task and
 * finish with the buffer before returning, so they share safely; the
 * storage_save_* diversion layer keeps its OWN scratch because it is
 * called BY these users with this buffer as the source. Callers wipe
 * after use (keystore_wipe / memset).
 */
typedef struct {
    union {
        conn_profile_t profiles[STORAGE_MAX_PROFILES];  /* ~1.9 KB */
        wifi_profile_t nets[STORAGE_WIFI_MAX];          /* ~0.8 KB */
    } u;                       /* phases within one job never overlap */
    wifi_profile_t one;        /* single-credential staging (migration) */
} storage_cred_scratch_t;

storage_cred_scratch_t *storage_cred_scratch(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_CRED_H */
