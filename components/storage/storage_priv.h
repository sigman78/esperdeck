/*
 * storage_priv.h — component-internal helpers shared between storage.c and
 * keystore.c. Not part of the public API.
 */

#ifndef STORAGE_PRIV_H
#define STORAGE_PRIV_H

#include "storage.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Scan <mount>/keys/ for files ending in @p ext (e.g. ".pem", ".kw1") and
 * append their stems to @p out, skipping stems already present (dedupe) and
 * stems that don't fit STORAGE_KEY_ID_LEN. *count is read AND updated —
 * pass 0 for a fresh scan, or chain calls to build a union. Unsorted.
 */
esp_err_t storage_scan_key_ext(const char *ext,
                               char (*out)[STORAGE_KEY_ID_LEN], int max,
                               int *count);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_PRIV_H */
