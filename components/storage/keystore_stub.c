/*
 * keystore_stub.c — inert stand-in for the secure store (keystore.c).
 *
 * Compiled when CONFIG_CYBERDECK_KEYSTORE is disabled or when the vault
 * implementation is absent from the tree (closed-source builds strip it).
 * Every answer is the ABSENT-store answer, which the rest of the firmware
 * already treats as "feature off, plaintext behavior" — no caller needs a
 * single #ifdef. keystore_wipe stays real: open code uses it to scrub
 * transient credential buffers regardless of the vault's presence.
 */

#include "keystore.h"
#include <string.h>

keystore_state_t keystore_state(void)  { return KEYSTORE_ABSENT; }
uint8_t          keystore_pin_len(void){ return 0; }

esp_err_t keystore_create(const char *pin)
{
    (void)pin;
    return ESP_ERR_INVALID_STATE;   /* vault not in this build */
}

esp_err_t keystore_unlock(const char *pin)
{
    (void)pin;
    return ESP_ERR_NOT_FOUND;              /* same as: no store present */
}

void keystore_lock(void)        {}
void keystore_reset_cache(void) {}

esp_err_t keystore_change_pin(const char *old_pin, const char *new_pin)
{
    (void)old_pin; (void)new_pin;
    return ESP_ERR_NOT_FOUND;
}

esp_err_t keystore_wrap(const char *key_id, uint8_t content_type,
                        const void *plaintext, size_t len)
{
    (void)key_id; (void)content_type; (void)plaintext; (void)len;
    return ESP_ERR_INVALID_STATE;
}

esp_err_t keystore_unwrap(const char *key_id, void *buf, size_t buf_len,
                          size_t *written, uint8_t *content_type)
{
    (void)key_id; (void)buf; (void)buf_len; (void)content_type;
    if (written) *written = 0;
    return ESP_ERR_NOT_FOUND;
}

bool keystore_is_wrapped(const char *key_id)
{
    (void)key_id;
    return false;
}

esp_err_t keystore_remove(const char *pin)
{
    (void)pin;
    return ESP_ERR_NOT_FOUND;
}

void keystore_wipe(void *buf, size_t len)
{
    /* Real scrubbing even without the vault: volatile so the store
     * cannot be elided as dead. */
    volatile unsigned char *p = (volatile unsigned char *)buf;
    if (!p) return;
    while (len--) *p++ = 0;
}

uint32_t keystore_backoff_ms(void) { return 0; }
void keystore_set_uptime_hook(uint64_t (*fn)(void)) { (void)fn; }

esp_err_t keystore_secret_get(const char *skey, char *out, size_t out_len)
{
    (void)skey;
    if (out && out_len) out[0] = '\0';
    return ESP_ERR_INVALID_STATE;
}

esp_err_t keystore_secret_set(const char *skey, const char *value)
{
    (void)skey; (void)value;
    return ESP_ERR_INVALID_STATE;
}

void keystore_secrets_prune(const char *prefix,
                            const char *const *keep, int nkeep)
{
    (void)prefix; (void)keep; (void)nkeep;
}
