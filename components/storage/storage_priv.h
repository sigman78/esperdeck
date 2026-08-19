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

/**
 * Best-effort secure delete: overwrite the file's bytes with zeros, flush,
 * then remove(). NOT a guaranteed erase on LittleFS — copy-on-write
 * wear-leveling can retire the old blocks untouched until reuse (the real
 * fix is the flash-encryption endgame, docs/storage_auth.md) — but it
 * shortens the exposure window and IS effective on the host sim's in-place
 * filesystems, where provisioning leaves plaintext keys behind.
 */
esp_err_t storage_shred_file(const char *path);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_PRIV_H */
