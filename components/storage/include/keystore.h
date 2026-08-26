/*
 * keystore.h — PIN-unlocked wrapped key store (docs/storage_auth.md).
 *
 * A random 32-byte master key (MK) wraps every key file; the MK itself is
 * stored in <mount>/keystore.kv1, wrapped by Argon2id(PIN, salt) in one of
 * up to four slots (LUKS-style indirection). Key files live beside the
 * legacy plaintext ones as keys/<id>.kw1 (XChaCha20-Poly1305, AAD binds
 * key id + store uuid, so renaming or transplanting a file breaks the tag).
 *
 * Compiled on BOTH device and simulator; the file formats are byte-identical
 * (explicit little-endian serialization, Monocypher primitives from the
 * libssh2 fork). No keystore.kv1 present = feature off.
 *
 * Error conventions:
 *   ESP_ERR_NOT_FOUND      — no store / no wrapped key file
 *   ESP_ERR_INVALID_STATE  — operation needs an unlocked store (or store
 *                            already exists, for keystore_create)
 *   ESP_FAIL               — wrong PIN (no slot unwrapped; header itself ok)
 *   ESP_ERR_INVALID_CRC    — corrupt/tampered store header or key file
 *   ESP_ERR_INVALID_SIZE   — caller buffer too small for the plaintext
 *   KEYSTORE_ERR_BACKOFF   — failed-attempt wait active; retry after
 *                            keystore_backoff_ms()
 */

#ifndef KEYSTORE_H
#define KEYSTORE_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* content_type values for wrapped files */
#define KEYSTORE_CONTENT_PEM     1   /* PEM private key */
#define KEYSTORE_CONTENT_SECRETS 2   /* secrets bundle (see below) */

/* Reserved key id for the secrets bundle — keys/secrets.kw1. Never listed
 * by storage_list_keys; do not name an SSH key "secrets". */
#define KEYSTORE_SECRETS_ID "secrets"

/* PIN/passphrase length accepted by unlock/create (bytes, not digits) */
#define KEYSTORE_PIN_MIN 1
#define KEYSTORE_PIN_MAX 64

/* Returned by unlock/change_pin/remove while a failed-attempt wait is
 * active (ESP_FAIL is -1; this is deliberately outside the ESP_ERR_ set). */
#define KEYSTORE_ERR_BACKOFF ((esp_err_t)-2)

typedef enum {
    KEYSTORE_ABSENT = 0,     /* no keystore.kv1 — feature off              */
    KEYSTORE_LOCKED,         /* store present, MK not in RAM               */
    KEYSTORE_UNLOCKED,       /* MK in RAM (internal SRAM), keys readable   */
} keystore_state_t;

/** Current store state. Never fails; a corrupt header reads as LOCKED
 *  (unlock will then report ESP_ERR_INVALID_CRC). */
keystore_state_t keystore_state(void);

/**
 * Configured PIN length for the pad's auto-submit, readable while LOCKED
 * (it is a header hint, not a secret — knowing it costs an attacker <10 %
 * of the search space). 0 = unknown (pre-hint store) or passphrase-only:
 * the unlock UI then submits on Enter alone.
 */
uint8_t keystore_pin_len(void);

/**
 * Create a new store: random MK + uuid, @p pin wrapped into slot 0.
 * The store is left UNLOCKED. Refuses to overwrite an existing store
 * (ESP_ERR_INVALID_STATE) — delete keystore.kv1 first if you mean it.
 */
esp_err_t keystore_create(const char *pin);

/**
 * Derive the KEK from @p pin and try every occupied slot. On success the MK
 * stays in RAM (state UNLOCKED) and any bare keys/<id>.pem files are adopted:
 * wrapped to .kw1 and deleted (best-effort migration).
 * ESP_OK also when already unlocked.
 */
esp_err_t keystore_unlock(const char *pin);

/** Wipe the in-RAM MK. No-op when not unlocked. */
void keystore_lock(void);

/**
 * Re-wrap the slot that @p old_pin opens with @p new_pin (fresh salt+nonce).
 * Key files are untouched. Works locked or unlocked; @p old_pin must be
 * correct either way.
 */
esp_err_t keystore_change_pin(const char *old_pin, const char *new_pin);

/**
 * Wrap @p len bytes of plaintext into keys/<key_id>.kw1 (atomic replace).
 * Requires UNLOCKED.
 */
esp_err_t keystore_wrap(const char *key_id, uint8_t content_type,
                        const void *plaintext, size_t len);

/**
 * Unwrap keys/<key_id>.kw1 into @p buf. *written = plaintext length (no NUL
 * added — the payload may be binary). @p content_type may be NULL.
 * Requires UNLOCKED (ESP_ERR_INVALID_STATE otherwise).
 */
esp_err_t keystore_unwrap(const char *key_id, void *buf, size_t buf_len,
                          size_t *written, uint8_t *content_type);

/** True if keys/<key_id>.kw1 exists. */
bool keystore_is_wrapped(const char *key_id);

/**
 * Decommission the store: prove @p pin (even when already unlocked — this
 * writes private keys back as plaintext), unwrap every keys/<id>.kw1 to a
 * bare keys/<id>.pem, then delete keystore.kv1 (state ABSENT, feature off).
 * A key that fails to unwrap aborts BEFORE the header is deleted; a crash
 * mid-way is safe — the next unlock re-adopts the emitted .pem files.
 * ESP_FAIL = wrong code.
 */
esp_err_t keystore_remove(const char *pin);

/**
 * Forget cached header state (call after deleting keystore.kv1 externally,
 * e.g. factory reset). Also wipes the MK like keystore_lock().
 */
void keystore_reset_cache(void);

/**
 * crypto_wipe wrapper for callers holding transient plaintext (unwrapped
 * PEM buffers, typed codes) outside this component — a zeroing the
 * compiler cannot elide, unlike a trailing memset.
 */
void keystore_wipe(void *buf, size_t len);

/* -------------------------------------------------------------------------
 * Secrets bundle — keys/secrets.kw1 (content_type 2)
 *
 * One wrapped blob of "namespace:key=value" lines holding every non-key
 * credential: "profile:<name>=<password-or-passphrase>" and
 * "wifi:<ssid>=<psk>". Cached in internal SRAM with exactly the master
 * key's lifetime (loaded lazily after unlock, wiped at lock). Values may
 * not contain newlines; keys may not contain '=' or newlines.
 * ---------------------------------------------------------------------- */

/**
 * Fetch a secret into @p out (NUL-terminated). ESP_ERR_INVALID_STATE while
 * locked, ESP_ERR_NOT_FOUND when absent, ESP_ERR_INVALID_SIZE if @p out is
 * too small.
 */
esp_err_t keystore_secret_get(const char *skey, char *out, size_t out_len);

/**
 * Add/replace a secret and rewrap the bundle (NULL or "" value removes the
 * entry; an emptied bundle deletes keys/secrets.kw1). Requires UNLOCKED.
 */
esp_err_t keystore_secret_set(const char *skey, const char *value);

/**
 * Drop every "<prefix><name>=" entry whose <name> is not in @p keep —
 * e.g. prune "profile:" entries after a profile delete/rename. Rewraps
 * only when something was removed. No-op while locked.
 */
void keystore_secrets_prune(const char *prefix,
                            const char *const *keep, int nkeep);

/**
 * Remaining failed-attempt wait in ms; 0 = attempts allowed now. The
 * counter (backoff.cnt) survives reboots; the wait itself runs on
 * monotonic uptime, so a reboot re-arms the current delay in full.
 */
uint32_t keystore_backoff_ms(void);

/**
 * Test seam: replace the monotonic-uptime source (NULL restores the real
 * one). Resets the cached backoff state so the boot penalty re-derives.
 */
void keystore_set_uptime_hook(uint64_t (*fn)(void));

#endif /* KEYSTORE_H */
