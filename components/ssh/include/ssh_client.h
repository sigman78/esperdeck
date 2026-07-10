/*
 * SSH client interface
 */

#ifndef SSH_CLIENT_H
#define SSH_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* Distinct connect failures the shell can react to (beyond ESP_FAIL). */
#define SSH_ERR_BASE              0x7000
#define SSH_ERR_HOSTKEY_UNKNOWN   (SSH_ERR_BASE + 1)  /* no pinned fp, TOFU prompt needed */
#define SSH_ERR_HOSTKEY_MISMATCH  (SSH_ERR_BASE + 2)  /* pinned fp does not match!        */
#define SSH_ERR_AUTH              (SSH_ERR_BASE + 3)  /* all auth methods failed          */

// SSH configuration
typedef struct {
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;     // Password auth (when private_key is NULL)
    const char *private_key;  // Path to a PEM private key → public-key auth
    const char *public_key;   // Optional .pub; needed for ECDSA (the mbedTLS
                              // backend can only derive an RSA pubkey from priv)
    const char *passphrase;   // Optional: decrypts an encrypted private_key
    /*
     * Pinned host-key fingerprint (lowercase hex SHA256, 64 chars) from a
     * previous session.  NULL = unknown host: the connect stops after the
     * handshake with SSH_ERR_HOSTKEY_UNKNOWN so the caller can show a
     * trust-on-first-use prompt, then retry with the fingerprint set.
     */
    const char *expected_fp;
} ssh_config_t;

/**
 * Initialize SSH client
 */
esp_err_t ssh_client_init(void);

/**
 * Connect to SSH server (TCP, handshake, host-key check, auth, PTY, shell).
 *
 * @return ESP_OK, SSH_ERR_HOSTKEY_UNKNOWN, SSH_ERR_HOSTKEY_MISMATCH,
 *         SSH_ERR_AUTH, or ESP_FAIL.
 */
esp_err_t ssh_client_connect(const ssh_config_t *config);

/**
 * Asynchronous connect: run ssh_client_connect() on a worker task so the UI
 * stays live during DNS/TCP/handshake/auth. The caller MUST keep the strings
 * referenced by @p config alive until the result is taken (the struct is
 * shallow-copied). Returns ESP_OK once the worker is launched, or
 * ESP_ERR_INVALID_STATE if a connect is already in flight.
 */
esp_err_t ssh_client_connect_start(const ssh_config_t *config);

/** True while an async connect is still running. */
bool ssh_client_connect_pending(void);

/** True once the async connect has finished (result available). */
bool ssh_client_connect_ready(void);

/** Take the async connect result (same codes as ssh_client_connect) and reset
 *  the async state. Call once, after ssh_client_connect_ready() is true. */
esp_err_t ssh_client_connect_take_result(void);

/** Best-effort abort of an in-flight async connect (unblocks a stalled
 *  handshake; the worker still finishes and reports an error). */
void ssh_client_connect_cancel(void);

/**
 * Disconnect from SSH server
 */
esp_err_t ssh_client_disconnect(void);

/**
 * Send data to SSH server
 *
 * @param data Data to send
 * @param len Data length
 * @return Number of bytes sent, or -1 on error
 */
int ssh_client_send(const uint8_t *data, size_t len);

/**
 * Check if connected
 *
 * @return true if connected
 */
bool ssh_client_is_connected(void);

/**
 * SHA256 host-key fingerprint (64 hex chars) observed during the most
 * recent connect attempt — valid after ESP_OK, SSH_ERR_HOSTKEY_UNKNOWN
 * and SSH_ERR_HOSTKEY_MISMATCH. Empty string otherwise.
 */
const char *ssh_client_get_fingerprint(void);

/**
 * Human-readable description of the most recent connect failure.
 */
const char *ssh_client_last_error(void);

#endif // SSH_CLIENT_H
