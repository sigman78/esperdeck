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
 * True if @p key_id is safe to interpolate into a keys/<id>.<ext> path:
 * non-empty, fits STORAGE_KEY_ID_LEN, and carries no path separator, no
 * Windows drive letter / NTFS alternate-data-stream colon.
 *
 * Every entry point that builds a key path must gate on this. key_id values
 * are NOT all self-generated: they come back from profiles.ini, a plain text
 * file that a hand edit, a restored backup or a pre-sanitizer profile can put
 * "../../keystore" into — which would otherwise turn storage_delete_key()
 * into an arbitrary unlink and storage_get_key() into an arbitrary read.
 */
bool storage_key_id_ok(const char *key_id);

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

/**
 * Raw ini writers — the bodies of storage_save_profiles/storage_wifi_save
 * WITHOUT the secrets-bundle diversion. Used by keystore.c's remove-code
 * restore, which must write plaintext back on purpose.
 */
esp_err_t storage_profiles_write_raw(const conn_profile_t *profiles, int count);
esp_err_t storage_wifi_write_raw(const wifi_profile_t *profiles, int count);

/**
 * True if profiles.ini or wifi.ini still carries a non-empty plaintext
 * password= value — i.e. credentials written while the store was locked
 * or absent, waiting for bundle adoption at the next unlock.
 */
bool storage_secrets_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_PRIV_H */
