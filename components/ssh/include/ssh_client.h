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

/*
 * Byte sink — where the read task delivers remote output (the seam
 * that keeps the transport terminal-free; extensibility item 7).
 * data() parses one chunk on the read task with the session lock
 * held. It may call ssh_client_queue_reply(), and nothing else on
 * this API. flush() runs once after each drained batch. closed()
 * fires when the session ends, clean or not.
 */
typedef struct {
    void (*data)(const char *buf, size_t len, void *user);
    void (*flush)(void *user);
    void (*closed)(bool clean_eof, void *user);
    void *user;
} ssh_sink_t;

// SSH configuration
typedef struct {
    const char *host;
    uint16_t port;
    const char *username;
    const char *password;     // Password auth (when private_key_pem is NULL)
    /*
     * Key auth passes PEM CONTENTS, not file paths. The connect worker
     * runs on a PSRAM stack. Any flash I/O there, including littlefs
     * fopen, asserts inside spi_flash_disable_interrupts_caches_and_
     * other_cpu(). The caller — the shell task, on an internal stack —
     * reads the files instead.
     */
    const char *private_key_pem;  // NUL-terminated PEM key → public-key auth
    const char *public_key_pem;   // Optional pubkey contents; needed for ECDSA
                                  // (the mbedTLS backend derives the pubkey
                                  // from priv only for RSA and ed25519 keys)
    const char *passphrase;   // Optional: decrypts an encrypted private key
    /*
     * Pinned host-key fingerprint (lowercase hex SHA256, 64 chars) from a
     * previous session. NULL means an unknown host. The connect then stops
     * after the handshake with SSH_ERR_HOSTKEY_UNKNOWN. This lets the
     * caller show a trust-on-first-use prompt, then retry with the
     * fingerprint set.
     */
    const char *expected_fp;
    /* Remote-output consumer; required. Must outlive the session. */
    const ssh_sink_t *sink;
    /* PTY geometry for the remote; 0,0 falls back to 80x24. */
    uint16_t term_cols, term_rows;
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
 * Runs ssh_client_connect() on a worker task, asynchronously, so the UI
 * stays live during DNS, TCP, handshake, and auth. The caller MUST keep
 * the strings referenced by @p config alive until it takes the result
 * (the struct is shallow-copied). Returns ESP_OK once the worker launches,
 * or ESP_ERR_INVALID_STATE if a connect is already in flight.
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
 * Queue terminal-response bytes (DA1, cursor reports) for the remote.
 * Call ONLY from inside the sink's data() callback — the read loop
 * drains the queue with non-blocking writes. Overflow drops the tail.
 */
void ssh_client_queue_reply(const uint8_t *data, size_t len);

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
 * True when the last session ended because the remote closed the channel
 * cleanly (orderly EOF — e.g. `exit` at the remote shell), as opposed to a
 * transport error. Valid once ssh_client_is_connected() has gone false;
 * reset on the next connect attempt.
 */
bool ssh_client_session_eof(void);

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
