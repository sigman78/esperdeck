/*
 * ssh_client.c — libssh2-based SSH client for the Cyberdeck terminal.
 *
 * Lifecycle:
 *   ssh_client_connect()  — TCP connect, SSH handshake, auth, PTY, shell
 *   ssh_client_send()     — write bytes to the remote shell
 *   ssh_client_is_connected() — poll connection state
 *   ssh_client_disconnect() — clean shutdown
 *
 * A dedicated FreeRTOS task (ssh_read_task, core 0) blocks on
 * libssh2_channel_read() and feeds output into vterm.  The session is
 * switched to non-blocking mode after the shell is opened so that the
 * read task can detect EOF/errors promptly.
 */

#include "ssh_client.h"
#include "vterm.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "libssh2.h"

#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#define drain_now_us()  esp_timer_get_time()
#if CONFIG_SSH_WIFI_PS_NONE
#include "esp_wifi.h"
#endif
#else
#define drain_now_us()  0   /* simulator: byte budget only bounds the drain */
#endif

#ifndef SHUT_RDWR          /* Winsock spells it SD_BOTH; both are 2 */
#define SHUT_RDWR 2
#endif

static const char *TAG = "ssh_client";

/* -------------------------------------------------------------------------
 * Custom libssh2 allocators — prefer SPIRAM for session/channel/packet
 * buffers so that fragmented internal DRAM is not exhausted by large SSH
 * receive bursts.  Falls back to internal DRAM when SPIRAM is unavailable.
 *
 * s_alloc_bytes tracks live bytes held by libssh2 at any moment.
 * heap_caps_get_allocated_size() returns the actual block size so the
 * counter reflects real heap consumption including allocator overhead.
 * ---------------------------------------------------------------------- */
static volatile size_t s_alloc_bytes = 0;

static void *ssh_malloc(size_t size, void **abstract)
{
    (void)abstract;
    void *p = heap_caps_malloc(size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    if (p) s_alloc_bytes += heap_caps_get_allocated_size(p);
    return p;
}

static void *ssh_realloc(void *ptr, size_t size, void **abstract)
{
    (void)abstract;
    size_t old_size = ptr ? heap_caps_get_allocated_size(ptr) : 0;
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT | MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_realloc(ptr, size, MALLOC_CAP_8BIT);
    if (p) {
        s_alloc_bytes -= old_size;
        s_alloc_bytes += heap_caps_get_allocated_size(p);
    }
    /* On failure ptr is still valid; its size remains counted — no change. */
    return p;
}

static void ssh_free(void *ptr, void **abstract)
{
    (void)abstract;
    if (ptr) {
        s_alloc_bytes -= heap_caps_get_allocated_size(ptr);
        heap_caps_free(ptr);
    }
}

/* Password stashed during connection for the kbd-interactive callback. */
static const char *s_kb_password = NULL;

/* -------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------- */
static LIBSSH2_SESSION  *s_session    = NULL;
static LIBSSH2_CHANNEL  *s_channel    = NULL;
static int               s_sock       = -1;
static TaskHandle_t      s_read_task  = NULL;
static volatile bool     s_connected  = false;
static volatile bool     s_read_task_done = true;   /* read task has exited */
static bool              s_libssh2_initialized = false;

/* ssh_read_task runs in PSRAM: internal DRAM is scarce once WiFi + NimBLE +
 * the display overlay are up, and a dynamic 8 KB internal stack alloc fails
 * (that is the "Failed to create ssh_read_task" error). The task only does
 * sockets + crypto + vterm writes — no flash/ISR work — so an external-RAM
 * stack is safe. TCB stays in internal DRAM; the stack buffer is reused
 * across sessions. */
#define SSH_READ_STACK_BYTES 8192
static StaticTask_t      s_read_task_tcb;
static StackType_t      *s_read_task_stack = NULL;

/* Drain-loop tuning (docs/speedup-render.md #1). Chunks land in a PSRAM
 * buffer — only this task touches it (sockets + crypto, no flash I/O), and
 * 2 KB of internal DRAM is too precious. Reused across sessions. The budget
 * bounds core-0 CPU per wake so IDLE0 (task WDT) and the same-core input
 * tasks (touch prio 4, uart prio 5 — which this task, prio 6, preempts
 * outright) always get to run. 8 KB per 10 ms tick ≈ 800 KB/s ceiling. */
#define SSH_READ_CHUNK       2048
#define SSH_DRAIN_BUDGET     8192      /* bytes per wake */
#define SSH_DRAIN_BUDGET_US  5000      /* time bound per wake */
static char             *s_read_buf = NULL;

/* Async connect worker — runs the blocking ssh_client_connect() off the UI
 * task so the shell stays responsive. PSRAM stack (crypto handshake is heavy;
 * no flash writes, only key-file reads, so external RAM is fine). */
#define SSH_CONNECT_STACK_BYTES 12288
static StaticTask_t      s_connect_tcb;
static StackType_t      *s_connect_stack = NULL;
static TaskHandle_t      s_connect_task  = NULL;
static ssh_config_t      s_pending_cfg;             /* shallow copy; strings owned by caller */
static volatile int      s_connect_phase = 0;       /* 0 idle, 1 pending, 2 done */
static volatile esp_err_t s_connect_result = ESP_FAIL;

/*
 * Serializes all libssh2 session/channel calls. libssh2 is not thread-safe
 * per session, and ssh_read_task (core 0) races ssh_client_send (core 1).
 * The vterm response callback runs inside the read task's tsm_feed while
 * the read task already holds the lock — it must NOT take it again.
 */
static SemaphoreHandle_t s_session_lock = NULL;

/* Pending terminal-response bytes (DA1 / cursor-position replies that tsm
 * emits while parsing during vterm_write). These MUST NOT be written to
 * libssh2 from the response callback: it runs inside the read task with
 * s_session_lock held, and a blocking write there could park the read task
 * holding the lock — which, if disconnect() then force-deletes the task,
 * orphans the mutex and wedges all future SSH I/O. Instead we buffer them and
 * drain non-blocking from the read loop (still under the lock). Responses are
 * a handful of bytes; 256 is plenty. Only ever touched under s_session_lock. */
#define SSH_RESP_BUF 256
static uint8_t s_resp_buf[SSH_RESP_BUF];
static size_t  s_resp_len = 0;

/* Bound on any blocking libssh2 call (handshake/auth). Non-blocking calls in
 * the read loop are unaffected — they return EAGAIN immediately. */
#define SSH_BLOCKING_TIMEOUT_MS 20000

/* Fingerprint of the most recent connect attempt + last error message. */
static char s_fingerprint[65] = "";
static char s_last_error[96]  = "";

static void set_last_error(const char *msg)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg ? msg : "");
}

/* -------------------------------------------------------------------------
 * ssh_cleanup — release all libssh2 and socket resources
 * ---------------------------------------------------------------------- */
static void ssh_cleanup(void)
{
    if (s_channel) {
        libssh2_channel_close(s_channel);
        libssh2_channel_free(s_channel);
        s_channel = NULL;
    }
    if (s_session) {
        libssh2_session_disconnect(s_session, "bye");
        libssh2_session_free(s_session);
        s_session = NULL;
    }
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_connected = false;
    s_resp_len  = 0;
    ESP_LOGI(TAG, "SSH cleanup done  — libssh2 heap: %zu B (expect 0)", s_alloc_bytes);
}

/* -------------------------------------------------------------------------
 * vterm response callback — sends terminal responses (e.g. DA1, cursor
 * position reports) from tsm back to the remote over SSH.
 * ---------------------------------------------------------------------- */
static void ssh_vterm_response_cb(const char *data, size_t len, void *user)
{
    (void)user;
    if (!s_connected || len == 0) return;
    /* Runs inside the read loop with s_session_lock held. Only buffer here —
     * never write to libssh2 (see s_resp_buf note). Drop the overflow tail:
     * the buffer only fills if the uplink is wedged, in which case the
     * session is about to drop anyway. */
    size_t room = sizeof(s_resp_buf) - s_resp_len;
    if (len > room) len = room;
    if (len) {
        memcpy(s_resp_buf + s_resp_len, data, len);
        s_resp_len += len;
    }
}

/* Flush buffered terminal responses with NON-blocking writes. Caller must hold
 * s_session_lock and the session must be in non-blocking mode. Anything not
 * sent (EAGAIN) stays buffered for the next read-loop iteration. */
static void drain_responses(void)
{
    while (s_resp_len > 0 && s_channel) {
        ssize_t rc = libssh2_channel_write(s_channel,
                                           (const char *)s_resp_buf, s_resp_len);
        if (rc == LIBSSH2_ERROR_EAGAIN) break;   /* retry next iteration */
        if (rc < 0) { s_resp_len = 0; break; }   /* unrecoverable — drop */
        if ((size_t)rc >= s_resp_len) { s_resp_len = 0; break; }
        memmove(s_resp_buf, s_resp_buf + rc, s_resp_len - (size_t)rc);
        s_resp_len -= (size_t)rc;
    }
}

/* -------------------------------------------------------------------------
 * Log the last libssh2 error string for the current session.
 * ---------------------------------------------------------------------- */
static void log_last_error(const char *context)
{
    char *errmsg = NULL;
    libssh2_session_last_error(s_session, &errmsg, NULL, 0);
    ESP_LOGE(TAG, "%s: %s", context, errmsg ? errmsg : "unknown");

    char buf[96];
    snprintf(buf, sizeof(buf), "%s: %s", context, errmsg ? errmsg : "unknown");
    set_last_error(buf);
}

/* -------------------------------------------------------------------------
 * ssh_read_task — pinned to core 0, feeds remote output into vterm.
 *
 * Each wake drains the channel until EAGAIN or the per-wake budget is spent
 * (SSH_DRAIN_BUDGET bytes / SSH_DRAIN_BUDGET_US), presents the batch once,
 * then ALWAYS yields at least one tick — libssh2_channel_read() can return
 * data continuously and never block; without the yield IDLE0 is starved and
 * the task watchdog fires. One 512 B read per tick used to cap ingest at
 * ~51 KB/s, which made every fullscreen redraw crawl (docs/speedup-render.md).
 *
 * The session lock is given back between chunks so ssh_client_send (shell
 * task, core 1) interleaves mid-batch — key echo waits one chunk, not a
 * whole drain.
 *
 * Keepalive packets are sent via libssh2_keepalive_send() which internally
 * tracks timing; we call it on every EAGAIN idle cycle. Do not move that
 * call: keepalive.c sends from a stack-local buffer and a blocked send is
 * only reissued correctly from the same call path.
 * ---------------------------------------------------------------------- */
static void ssh_read_task(void *arg)
{
    TickType_t last_stat = xTaskGetTickCount();

    while (s_connected) {
        size_t  drained = 0;
        int64_t wake_t0 = drain_now_us();
        int     n;

        do {
            xSemaphoreTake(s_session_lock, portMAX_DELAY);
            /* vterm_feed below may re-enter libssh2 via the response cb —
             * that is safe: same task, lock already held, cb does not
             * re-take. */
            n = libssh2_channel_read(s_channel, s_read_buf, SSH_READ_CHUNK);
            if (n > 0) {
                vterm_feed(s_read_buf, (size_t)n);  /* parse only; present below */
                drained += (size_t)n;
            } else if (n == LIBSSH2_ERROR_EAGAIN) {
                /* No data — good time to send a keepalive if one is due.
                 * NOTE: EAGAIN can also mean "data queued but the outbound
                 * WINDOW_ADJUST would block"; the staged adjust flushes on
                 * the next wake, so treat EAGAIN strictly as end-of-wake. */
#if CONFIG_SSH_KEEPALIVE_INTERVAL > 0
                int next_ka = 0;
                libssh2_keepalive_send(s_session, &next_ka);
#endif
            } else if (n == 0) {
                /* Zero-length read: orderly EOF if the channel says so
                 * (remote `exit`), else just an idle cycle. NOT an error —
                 * leaving the stale libssh2 message in last_error made every
                 * clean logout look like a failure to the shell. */
                if (libssh2_channel_eof(s_channel)) {
                    ESP_LOGI(TAG, "ssh_read_task: read EOF");
                    set_last_error("");
                    s_connected = false;
                }
            } else {
                /* Unrecoverable error */
                log_last_error("channel_read");
                s_connected = false;
            }

            /* Push out any terminal responses tsm queued above (non-blocking,
             * so this never parks the task while holding the lock). */
            if (s_connected) drain_responses();

            if (s_connected && libssh2_channel_eof(s_channel)) {
                ESP_LOGI(TAG, "ssh_read_task: channel EOF");
                s_connected = false;
            }
            xSemaphoreGive(s_session_lock);
        } while (s_connected && n > 0 &&
                 drained < SSH_DRAIN_BUDGET &&
                 (drain_now_us() - wake_t0) < SSH_DRAIN_BUDGET_US);

        /* Present the whole batch once (no-op while a ?2026 synchronized
         * update is open — btop frames land atomically). Present even if the
         * session just dropped so the tail of the output reaches the glass. */
        if (drained > 0)
            vterm_flush();

        if (!s_connected) break;

        /* Periodic memory stat. */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_stat) >= pdMS_TO_TICKS(30000)) {
            ESP_LOGI(TAG, "libssh2 heap: %zu B", s_alloc_bytes);
            last_stat = now;
        }

        /* Yield every wake so IDLE0 can run and reset the task WDT.
         * vTaskDelay(1) blocks for exactly one FreeRTOS tick regardless of
         * tick rate; sleep longer only when the link is idle. */
        vTaskDelay(drained > 0 ? 1 : pdMS_TO_TICKS(10));
    }

    s_read_task_done = true;   /* disconnect() polls this before cleanup */
    s_read_task = NULL;
    vTaskDelete(NULL);
}

/* -------------------------------------------------------------------------
 * Keyboard-interactive response callback.
 * Responds to every prompt with s_kb_password (set before auth attempt).
 * libssh2 frees responses[i].text (via ssh_free) after the callback returns,
 * so allocate through ssh_malloc — otherwise the free decrements s_alloc_bytes
 * for a block that was never counted, corrupting the leak gauge.
 * ---------------------------------------------------------------------- */
static LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(kbd_callback)
{
    (void)name; (void)name_len;
    (void)instruction; (void)instruction_len;
    (void)abstract;
    const char *pwd = s_kb_password ? s_kb_password : "";
    size_t len = strlen(pwd);
    for (int i = 0; i < num_prompts; i++) {
        char *buf = ssh_malloc(len + 1, NULL);   /* counted; freed via ssh_free */
        if (!buf) { responses[i].text = NULL; responses[i].length = 0; continue; }
        memcpy(buf, pwd, len + 1);
        responses[i].text   = buf;
        responses[i].length = (unsigned int)len;
    }
}

/* =========================================================================
 * Public API
 * ====================================================================== */

esp_err_t ssh_client_init(void)
{
    /* libssh2_init() is deferred to first connect; nothing to do here. */
    return ESP_OK;
}

esp_err_t ssh_client_connect(const ssh_config_t *config)
{
    if (!config || !config->host || !config->username) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_session_lock) {
        s_session_lock = xSemaphoreCreateMutex();
        if (!s_session_lock) return ESP_ERR_NO_MEM;
    }

    s_fingerprint[0] = '\0';
    s_last_error[0]  = '\0';

    /* ── 0. Release any leftover state from a previous session ──────── *
     * ssh_read_task sets s_connected=false on disconnect but does not    *
     * call ssh_cleanup() — that must happen here before we allocate new  *
     * libssh2 objects, otherwise session_init() fails with OOM.         */
    ssh_client_disconnect();

    /* ── 1. TCP connect ─────────────────────────────────────────────── */
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", (int)config->port);

    struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;

    ESP_LOGI(TAG, "Resolving %s:%s", config->host, port_str);
    if (getaddrinfo(config->host, port_str, &hints, &res) != 0 || res == NULL) {
        ESP_LOGE(TAG, "getaddrinfo failed for %s", config->host);
        return ESP_FAIL;
    }

    s_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        freeaddrinfo(res);
        return ESP_FAIL;
    }

    int tcpFlag = 1;
    setsockopt(s_sock, IPPROTO_TCP, TCP_NODELAY, &tcpFlag, sizeof(int));

    ESP_LOGI(TAG, "Connecting TCP socket...");
    if (connect(s_sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "TCP connect failed");
        freeaddrinfo(res);
        ssh_cleanup();
        return ESP_FAIL;
    }
    freeaddrinfo(res);
    ESP_LOGI(TAG, "TCP connected");

    /* ── 2. libssh2 init (once) ─────────────────────────────────────── */
    if (!s_libssh2_initialized) {
        if (libssh2_init(0) != 0) {
            ESP_LOGE(TAG, "libssh2_init failed");
            ssh_cleanup();
            return ESP_FAIL;
        }
        s_libssh2_initialized = true;
    }

    /* ── 3. Session init (custom SPIRAM-preferring allocators) ─────── */
    s_session = libssh2_session_init_ex(ssh_malloc, ssh_free, ssh_realloc, NULL);
    if (!s_session) {
        ESP_LOGE(TAG, "libssh2_session_init_ex failed");
        ssh_cleanup();
        return ESP_FAIL;
    }
    libssh2_session_set_blocking(s_session, 1);
    /* Inner errors (userauth masks them with "Callback returned error")
     * land on stderr → UART console. No-op stub unless the libssh2_esp
     * component was built with CONFIG_LIBSSH2_DEBUG_ENABLE. */
    libssh2_trace(s_session, LIBSSH2_TRACE_ERROR | LIBSSH2_TRACE_AUTH |
                             LIBSSH2_TRACE_PUBLICKEY);
    /* Never let the blocking handshake/auth hang forever on a dead link
     * (do_connect runs synchronously on the shell task). */
    libssh2_session_set_timeout(s_session, SSH_BLOCKING_TIMEOUT_MS);

    /* ── 4. SSH handshake ───────────────────────────────────────────── */
    ESP_LOGI(TAG, "SSH handshake...");
    int rc = libssh2_session_handshake(s_session, s_sock);
    if (rc != 0) {
        ESP_LOGE(TAG, "SSH handshake failed: %d", rc);
        log_last_error("handshake");
        ssh_cleanup();
        return ESP_FAIL;
    }

    /* ── 4b. Keepalive configuration ───────────────────────────────── */
#if CONFIG_SSH_KEEPALIVE_INTERVAL > 0
    /* want_reply=0: we send traffic to keep NAT alive but don't ask the
     * server to respond.  want_reply=1 causes the server to send back
     * SSH_MSG_REQUEST_SUCCESS which libssh2 must allocate a buffer for;
     * under heap pressure that allocation fails with LIBSSH2_ERROR_ALLOC
     * and drops the channel.                                              */
    libssh2_keepalive_config(s_session, 0 /* want_reply */,
                             CONFIG_SSH_KEEPALIVE_INTERVAL);
    ESP_LOGI(TAG, "Keepalive every %d s", CONFIG_SSH_KEEPALIVE_INTERVAL);
#endif

    /* ── 5. Host-key verification (TOFU pinning) ────────────────────── */
    const char *fp = libssh2_hostkey_hash(s_session, LIBSSH2_HOSTKEY_HASH_SHA256);
    if (!fp) {
        set_last_error("no host key hash");
        ssh_cleanup();
        return ESP_FAIL;
    }
    for (int i = 0; i < 32; i++)
        snprintf(s_fingerprint + i * 2, 3, "%02x", (unsigned char)fp[i]);
    ESP_LOGI(TAG, "Host fingerprint SHA256: %s", s_fingerprint);

    if (!config->expected_fp) {
        /* Unknown host: stop BEFORE sending credentials so the user can
         * confirm the fingerprint first. Caller re-connects with it set. */
        set_last_error("unknown host key");
        ssh_cleanup();
        return SSH_ERR_HOSTKEY_UNKNOWN;
    }
    if (strcmp(config->expected_fp, s_fingerprint) != 0) {
        ESP_LOGE(TAG, "HOST KEY MISMATCH for %s (expected %s)",
                 config->host, config->expected_fp);
        set_last_error("HOST KEY MISMATCH");
        ssh_cleanup();
        return SSH_ERR_HOSTKEY_MISMATCH;
    }

    /* ── 6. Authentication ──────────────────────────────────────────── */
    ESP_LOGI(TAG, "Authenticating as '%s'...", config->username);

    /* Query which methods the server accepts. */
    char *authlist = libssh2_userauth_list(s_session, config->username,
                                           (unsigned int)strlen(config->username));
    if (!authlist) {
        if (libssh2_userauth_authenticated(s_session)) {
            ESP_LOGI(TAG, "Server requires no authentication");
            goto auth_done;
        }
        log_last_error("userauth_list");
        ssh_cleanup();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Server auth methods: %s", authlist);

    rc = LIBSSH2_ERROR_AUTHENTICATION_FAILED;

    if (config->private_key_pem) {
        /* Key profile: public-key auth ONLY. The passphrase decrypts the key
         * (mbedTLS derives the public key from the private one), it is NOT a
         * login password — so never fall through to password auth with it. */
        if (!strstr(authlist, "publickey")) {
            ESP_LOGE(TAG, "server does not offer publickey (methods: %s)", authlist);
            set_last_error("server rejects publickey");
            ssh_cleanup();
            return SSH_ERR_AUTH;
        }
        ESP_LOGI(TAG, "Public-key auth (%u-byte PEM in memory)",
                 (unsigned)strlen(config->private_key_pem));
        rc = libssh2_userauth_publickey_frommemory(
                 s_session,
                 config->username, strlen(config->username),
                 config->public_key_pem,
                 config->public_key_pem ? strlen(config->public_key_pem) : 0,
                 config->private_key_pem, strlen(config->private_key_pem),
                 config->passphrase);
        if (rc == 0) goto auth_done;
        log_last_error("publickey");
        ssh_cleanup();
        return SSH_ERR_AUTH;
    }

    /* Password profile: password, then keyboard-interactive. */
    s_kb_password = config->password;  /* stash for kbd-interactive callback */

    if (strstr(authlist, "password")) {
        rc = libssh2_userauth_password(s_session, config->username,
                                       config->password ? config->password : "");
        if (rc == 0) goto auth_done;
        ESP_LOGW(TAG, "Password auth failed (%d)", rc);
    }

    /* Try keyboard-interactive (responds every prompt with the password). */
    if (strstr(authlist, "keyboard-interactive")) {
        rc = libssh2_userauth_keyboard_interactive(s_session, config->username,
                                                   kbd_callback);
        if (rc == 0) goto auth_done;
        ESP_LOGW(TAG, "KBD-interactive auth failed (%d)", rc);
    }

    s_kb_password = NULL;
    log_last_error("authentication");
    ssh_cleanup();
    return SSH_ERR_AUTH;

auth_done:
    s_kb_password = NULL;
    ESP_LOGI(TAG, "Authenticated");

    /* ── 7. Open shell channel ──────────────────────────────────────── */
    s_channel = libssh2_channel_open_ex(s_session,
                                        "session", sizeof("session") - 1,
                                        CONFIG_SSH_RECV_WINDOW,
                                        LIBSSH2_CHANNEL_PACKET_DEFAULT,
                                        NULL, 0);
    if (!s_channel) {
        ESP_LOGE(TAG, "Channel open failed");
        ssh_cleanup();
        return ESP_FAIL;
    }

    /* ── 8. Request PTY ─────────────────────────────────────────────── */
    rc = libssh2_channel_request_pty_ex(s_channel,
                                        "xterm-256color", 14,
                                        NULL, 0,
                                        CONFIG_TERMINAL_WIDTH,
                                        CONFIG_TERMINAL_HEIGHT,
                                        0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "PTY request failed: %d", rc);
        ssh_cleanup();
        return ESP_FAIL;
    }

    /* ── 9. Start shell ─────────────────────────────────────────────── */
    rc = libssh2_channel_shell(s_channel);
    if (rc != 0) {
        ESP_LOGE(TAG, "Shell request failed: %d", rc);
        ssh_cleanup();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Shell opened");

    /* ── 10. Switch to non-blocking and wire vterm responses ────────── */
    libssh2_session_set_blocking(s_session, 0);
    vterm_set_response_cb(ssh_vterm_response_cb, NULL);

    /* ── 11. Clear the terminal, then spawn the read task on core 0 ───── */
    /* Clear from THIS task before the read task exists: vterm has no lock, so
     * letting main_task's clear race the read task's feed inside tsm_feed on
     * two cores could corrupt the grid. Doing it here makes the read task the
     * session's sole vterm writer. */
    vterm_write("\x1b[2J\x1b[H", 7);

    s_connected = true;
    s_read_task_done = false;
    if (!s_read_task_stack)
        s_read_task_stack = heap_caps_malloc(SSH_READ_STACK_BYTES,
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_read_buf)
        s_read_buf = heap_caps_malloc(SSH_READ_CHUNK,
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_read_task_stack || !s_read_buf) {
        ESP_LOGE(TAG, "No PSRAM for ssh_read stack/buffer");
        s_connected = false;
        s_read_task_done = true;
        ssh_cleanup();
        return ESP_FAIL;
    }
    s_read_task = xTaskCreateStaticPinnedToCore(
        ssh_read_task, "ssh_read",
        SSH_READ_STACK_BYTES / sizeof(StackType_t), NULL, 6,
        s_read_task_stack, &s_read_task_tcb, 0);
    if (!s_read_task) {
        ESP_LOGE(TAG, "Failed to create ssh_read_task");
        s_connected = false;
        s_read_task_done = true;
        ssh_cleanup();
        return ESP_FAIL;
    }

#if CONFIG_SSH_WIFI_PS_NONE
    /* Full radio wakefulness for the session: the default modem-sleep gates
     * receive on the AP's DTIM beacon (tens of ms RTT), which caps the
     * window-bound throughput and every key echo. Restored on disconnect.
     * (With the BLE keyboard connected, coexistence still time-slices the
     * radio, so this removes only the DTIM-gated part of the latency.) */
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif

    ESP_LOGI(TAG, "SSH session ready — libssh2 heap: %zu B", s_alloc_bytes);
    return ESP_OK;
}

/* =========================================================================
 * Async connect — run the blocking connect on a worker task
 * ====================================================================== */
static void ssh_connect_worker(void *arg)
{
    (void)arg;
    s_connect_result = ssh_client_connect(&s_pending_cfg);
    s_connect_task   = NULL;
    s_connect_phase  = 2;   /* done — publish result last */
    vTaskDelete(NULL);
}

esp_err_t ssh_client_connect_start(const ssh_config_t *config)
{
    if (!config) return ESP_ERR_INVALID_ARG;
    if (s_connect_phase == 1) return ESP_ERR_INVALID_STATE;

    s_pending_cfg    = *config;     /* shallow: caller keeps the strings alive */
    s_connect_result = ESP_FAIL;
    s_connect_phase  = 1;

    if (!s_connect_stack)
        s_connect_stack = heap_caps_malloc(SSH_CONNECT_STACK_BYTES,
                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_connect_stack) { s_connect_phase = 0; return ESP_ERR_NO_MEM; }

    s_connect_task = xTaskCreateStaticPinnedToCore(
        ssh_connect_worker, "ssh_conn",
        SSH_CONNECT_STACK_BYTES / sizeof(StackType_t), NULL, 5,
        s_connect_stack, &s_connect_tcb, 0);
    if (!s_connect_task) { s_connect_phase = 0; return ESP_FAIL; }
    return ESP_OK;
}

bool ssh_client_connect_pending(void) { return s_connect_phase == 1; }
bool ssh_client_connect_ready(void)   { return s_connect_phase == 2; }

esp_err_t ssh_client_connect_take_result(void)
{
    esp_err_t r = s_connect_result;
    s_connect_phase = 0;
    return r;
}

void ssh_client_connect_cancel(void)
{
    /* Unblock a stalled socket read/write without closing the fd (the worker's
     * ssh_cleanup() owns the close). No effect if still resolving DNS. */
    if (s_sock >= 0) shutdown(s_sock, SHUT_RDWR);
}

esp_err_t ssh_client_disconnect(void)
{
    s_connected = false;

    /* Wait for the read task to exit on its own — it checks s_connected
     * every iteration (worst case one EAGAIN sleep + one lock hold). */
    for (int i = 0; i < 100 && !s_read_task_done; i++)
        vTaskDelay(pdMS_TO_TICKS(20));

    if (!s_read_task_done && s_read_task) {
        /* Should never happen now that the read task holds the lock only
         * across non-blocking calls. If it ever does, the killed task may
         * still own s_session_lock (FreeRTOS never releases a deleted task's
         * mutex), so recreate the lock or every later take() deadlocks.
         * Safe here: disconnect runs on the shell task, which is the only
         * other taker — so nobody is parked on the lock at this instant. */
        ESP_LOGE(TAG, "ssh_read_task did not exit — force-deleting");
        vTaskDelete(s_read_task);
        s_read_task = NULL;
        s_read_task_done = true;
        vSemaphoreDelete(s_session_lock);
        s_session_lock = xSemaphoreCreateMutex();
    }

    vterm_set_response_cb(NULL, NULL);
    ssh_cleanup();

#if CONFIG_SSH_WIFI_PS_NONE
    /* Session over — give the radio its power budget back. */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
#endif
    return ESP_OK;
}

int ssh_client_send(const uint8_t *data, size_t len)
{
    if (!s_connected || !s_channel || len == 0)
        return -1;

    ssize_t sent = 0;
    while ((size_t)sent < len) {
        if (!s_connected) return -1;   /* dropped while we were spinning */

        xSemaphoreTake(s_session_lock, portMAX_DELAY);
        ssize_t rc = s_channel
            ? libssh2_channel_write(s_channel,
                                    (const char *)data + sent,
                                    len - (size_t)sent)
            : LIBSSH2_ERROR_CHANNEL_CLOSED;
        xSemaphoreGive(s_session_lock);

        if (rc == LIBSSH2_ERROR_EAGAIN) {
            /* >= 1 tick even at 100 Hz — a 5 ms request would round to a
             * busy-yield and trip the idle-task watchdog on a dead link. */
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (rc < 0) {
            ESP_LOGW(TAG, "channel_write error: %d", (int)rc);
            return -1;
        }
        sent += rc;
    }
    return (int)sent;
}

bool ssh_client_is_connected(void)
{
    return s_connected;
}

const char *ssh_client_get_fingerprint(void)
{
    return s_fingerprint;
}

const char *ssh_client_last_error(void)
{
    return s_last_error;
}
