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
 * libssh2_channel_read() and hands output to the sink. ssh_client_connect()
 * switches the session to non-blocking mode after it opens the shell. This
 * lets the read task detect EOF and errors promptly.
 */

#include "ssh_client.h"
#include "ssh_txbuf.h"
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
#include <inttypes.h>

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

/* These allocators prefer SPIRAM for session, channel, and packet buffers.
 * This avoids exhausting fragmented internal DRAM during large SSH receive
 * bursts. They fall back to internal DRAM when SPIRAM is unavailable.
 *
 * s_alloc_bytes tracks the live bytes libssh2 holds at any moment.
 * heap_caps_get_allocated_size() returns the actual block size, so the
 * counter reflects real heap use, including allocator overhead. */
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

static LIBSSH2_SESSION  *s_session    = NULL;
static LIBSSH2_CHANNEL  *s_channel    = NULL;
static int               s_sock       = -1;
static TaskHandle_t      s_read_task  = NULL;
static volatile bool     s_connected  = false;
static uint16_t          s_term_cols, s_term_rows;   /* pty size connect() sent */
static volatile bool     s_clean_eof  = false;      /* remote closed channel (exit) */
static TickType_t        s_last_rx_tick;            /* last inbound activity (read task) */
static volatile bool     s_read_task_done = true;   /* read task has exited */
static bool              s_libssh2_initialized = false;

/* ssh_read_task runs in PSRAM. Internal DRAM grows scarce once WiFi,
 * NimBLE, and the display overlay start. A dynamic 8 KB internal stack
 * alloc then fails with "Failed to create ssh_read_task". The task only
 * touches sockets, crypto, and the sink parser, with no flash or ISR work.
 * An external-RAM stack is therefore safe. The TCB stays in internal
 * DRAM. The task reuses the stack buffer across sessions. */
#define SSH_READ_STACK_BYTES 8192
static StaticTask_t      s_read_task_tcb;
static StackType_t      *s_read_task_stack = NULL;

/* Drain-loop tuning (docs/performance.md pass 1, #1). Chunks land in a PSRAM
 * buffer — only this task touches it (sockets + crypto, no flash I/O), and
 * 2 KB of internal DRAM is too precious. Reused across sessions. The budget
 * bounds core-0 CPU per wake, so IDLE0 (task WDT) always gets to run. Touch
 * (priority 4) and UART (priority 5) also get to run. This task, at
 * priority 6, preempts both outright. 8 KB per 10 ms tick ≈ 800 KB/s
 * ceiling. */
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
 * The reply-queue call runs inside the read task's sink data() while
 * the read task already holds the lock — it must NOT take it again.
 */
static SemaphoreHandle_t s_session_lock = NULL;

/* The one ordered TX FIFO for keystrokes and terminal replies. Only code
 * that holds s_session_lock may touch it. Rationale: docs/ARCHITECTURE.md,
 * "SSH uplink and liveness". */
static ssh_txbuf_t s_tx;

/* A pending window-change pair (ssh_client_request_redraw): 0 idle, 1 narrow
 * size next, 2 real size next. Advanced under s_session_lock by redraw_pump(). */
static int s_redraw_step = 0;

/* This bounds any blocking libssh2 call (handshake or auth). It does not
 * affect non-blocking calls in the read loop; they return EAGAIN
 * immediately. */
#define SSH_BLOCKING_TIMEOUT_MS 20000

/* Slack on top of 3 silent keepalive intervals. A reply can trail its
 * request by one round trip. */
#define SSH_SILENCE_MARGIN_S 5

/* TCP connect budget. The poll slice bounds the Cancel latency. */
#define SSH_CONNECT_TIMEOUT_MS 10000
#define SSH_CONNECT_SLICE_MS   200
static volatile bool s_connect_cancel = false;

/* Fingerprint of the most recent connect attempt + last error message. */
static char s_fingerprint[65] = "";
static char s_last_error[96]  = "";

static void set_last_error(const char *msg)
{
    snprintf(s_last_error, sizeof(s_last_error), "%s", msg ? msg : "");
}

/* Bound on each blocking close/disconnect during teardown. Teardown runs on
 * the UI task, so a dead peer may cost at most a few of these. */
#define SSH_CLOSE_TIMEOUT_MS 1500

static void kill_link(bool *done)
{
    if (!*done && s_sock >= 0) shutdown(s_sock, SHUT_RDWR);
    *done = true;
}

/* The shell session is non-blocking: there, channel_close/free and
 * session_free return EAGAIN without freeing anything. So teardown switches
 * to blocking with a short timeout. libssh2 skips the close handshake only
 * for a socket it already saw SSH_MSG_DISCONNECT on, never for a silent
 * peer. So after any failed step shutdown() makes the next read fail
 * immediately, which lets the free calls run to completion. */
static void ssh_cleanup(void)
{
    bool shut = false;

    if (s_session) {
        libssh2_session_set_blocking(s_session, 1);
        libssh2_session_set_timeout(s_session, SSH_CLOSE_TIMEOUT_MS);
    }
    if (s_channel) {
        if (libssh2_channel_close(s_channel) != 0) kill_link(&shut);
        if (libssh2_channel_free(s_channel) != 0) {
            kill_link(&shut);
            if (libssh2_channel_free(s_channel) != 0)
                ESP_LOGW(TAG, "channel_free failed; session_free will retry");
        }
        s_channel = NULL;
    }
    if (s_session) {
        if (!shut && libssh2_session_disconnect(s_session, "bye") != 0) kill_link(&shut);
        if (libssh2_session_free(s_session) != 0) {
            kill_link(&shut);
            if (libssh2_session_free(s_session) != 0)
                ESP_LOGE(TAG, "session_free failed - leaking %zu B", s_alloc_bytes);
        }
        s_session = NULL;
    }
    if (s_sock >= 0) {
        close(s_sock);
        s_sock = -1;
    }
    s_connected = false;
    ssh_txbuf_reset(&s_tx);
    s_redraw_step = 0;
    ESP_LOGI(TAG, "SSH cleanup done  — libssh2 heap: %zu B (expect 0)", s_alloc_bytes);
}

/* The session's registered byte sink (from ssh_config_t). */
static const ssh_sink_t *s_sink;

void ssh_client_queue_reply(const uint8_t *data, size_t len)
{
    if (!s_connected || len == 0) return;
    /* The sink data() contract holds the lock here: append only, see s_tx.
     * Overflow drops the tail. */
    ssh_txbuf_append(&s_tx, data, len);
}

static ptrdiff_t tx_write(void *ctx, const uint8_t *data, size_t len)
{
    (void)ctx;
    if (!s_channel) return LIBSSH2_ERROR_CHANNEL_CLOSED;
    ssize_t rc = libssh2_channel_write(s_channel, (const char *)data, len);
    return rc == LIBSSH2_ERROR_EAGAIN ? SSH_TXBUF_AGAIN : (ptrdiff_t)rc;
}

/* Non-blocking flush of s_tx. Caller must hold s_session_lock and the
 * session must be in non-blocking mode. Bytes not sent (EAGAIN) stay at the
 * head for the next caller on either task. */
static void tx_flush(void)
{
    if (ssh_txbuf_flush(&s_tx, tx_write, NULL) < 0)
        ESP_LOGW(TAG, "channel_write failed; TX buffer dropped");
}

/* Advance a pending window-change request. Same locking rules as tx_flush.
 * libssh2 resumes a blocked request from channel state when called again. */
static void redraw_pump(void)
{
    while (s_redraw_step && s_channel) {
        const int cols = s_term_cols ? s_term_cols : 80;
        const int rows = s_term_rows ? s_term_rows : 24;
        int rc = libssh2_channel_request_pty_size(
                     s_channel, s_redraw_step == 1 ? cols - 1 : cols, rows);
        if (rc == LIBSSH2_ERROR_EAGAIN) return;
        if (rc != 0) {
            ESP_LOGW(TAG, "pty size request failed: %d", rc);
            s_redraw_step = 0;
            return;
        }
        s_redraw_step = (s_redraw_step == 1) ? 2 : 0;
    }
}

static void log_last_error(const char *context)
{
    char *errmsg = NULL;
    libssh2_session_last_error(s_session, &errmsg, NULL, 0);
    ESP_LOGE(TAG, "%s: %s", context, errmsg ? errmsg : "unknown");

    char buf[96];
    snprintf(buf, sizeof(buf), "%s: %s", context, errmsg ? errmsg : "unknown");
    set_last_error(buf);
}

/* ssh_client_connect() pins ssh_read_task to core 0. The task hands
 * remote output to the registered sink.
 *
 * Each wake drains the channel until EAGAIN, or until it spends the
 * per-wake budget (SSH_DRAIN_BUDGET bytes / SSH_DRAIN_BUDGET_US). It then
 * presents the batch once and ALWAYS yields at least one tick.
 * libssh2_channel_read() can return data continuously and never block.
 * Without the yield, IDLE0 starves and the task watchdog fires. See the
 * drain-loop history in docs/performance.md, pass 1.
 *
 * The task gives the session lock back between chunks, so ssh_client_send
 * (shell task, core 1) can interleave mid-batch. Key echo then waits one
 * chunk, not a whole drain.
 *
 * libssh2_keepalive_send() sends keepalive packets and tracks their timing
 * internally; this task calls it on every EAGAIN idle cycle. Do not move
 * that call. keepalive.c sends from a stack-local buffer. Only the same
 * call path can reissue a blocked send correctly.
 */
/* net_bench provides arrival-gap diagnostics for stutter hunting. A "data
 * wake" is a drain-loop pass that yields bytes. The gap between
 * consecutive data wakes shows how long the screen had nothing new.
 * During a steady stream (btop at 100 ms), gaps above ~250 ms count as
 * stalls. The counters report whether stalls happen and how bad they are.
 * The burst size shows whether data queued up behind the stall (network
 * hiccup) or trickled in late (sender paused). */
static int64_t  s_nb_last_data_us;
static uint32_t s_nb_gap_max_ms;
static uint32_t s_nb_gaps_250;      /* gaps in (250, 500] ms */
static uint32_t s_nb_gaps_500;      /* gaps > 500 ms */
static uint32_t s_nb_burst_max;     /* largest single-wake drain, bytes */
static uint32_t s_nb_data_wakes;

static void net_bench_reset(void)
{
    s_nb_gap_max_ms = 0;
    s_nb_gaps_250   = 0;
    s_nb_gaps_500   = 0;
    s_nb_burst_max  = 0;
    s_nb_data_wakes = 0;
}

static void ssh_read_task(void *arg)
{
    TickType_t last_stat = xTaskGetTickCount();
    uint32_t   idle_wakes = 0;

    /* Fresh counters per session — otherwise the first 30 s report blends
     * in everything accumulated since boot. */
    net_bench_reset();
    s_nb_last_data_us = 0;
    s_last_rx_tick = xTaskGetTickCount();

    while (s_connected) {
        size_t  drained = 0;
        int64_t wake_t0 = drain_now_us();
        int     n;

        /* Inbound-activity probe. libssh2 swallows keepalive replies, so
         * watch the socket itself before reading. Counts reads > 0 too. */
        fd_set rfds;
        struct timeval tv = { 0, 0 };
        FD_ZERO(&rfds);
        FD_SET(s_sock, &rfds);
        if (select(s_sock + 1, &rfds, NULL, NULL, &tv) > 0)
            s_last_rx_tick = xTaskGetTickCount();
#if CONFIG_SSH_KEEPALIVE_INTERVAL > 0
        if ((TickType_t)(xTaskGetTickCount() - s_last_rx_tick) >
            pdMS_TO_TICKS((3 * CONFIG_SSH_KEEPALIVE_INTERVAL + SSH_SILENCE_MARGIN_S) * 1000)) {
            ESP_LOGE(TAG, "no data for 3 keepalive intervals; dropping");
            set_last_error("Server not responding");
            s_connected = false;
            break;
        }
#endif

        do {
            xSemaphoreTake(s_session_lock, portMAX_DELAY);
            /* The sink data() below may re-enter libssh2 via
             * ssh_client_queue_reply() — that is safe: same task, lock
             * already held, the queue call does not re-take. */
            n = libssh2_channel_read(s_channel, s_read_buf, SSH_READ_CHUNK);
            if (n > 0) {
                s_sink->data(s_read_buf, (size_t)n, s_sink->user);
                drained += (size_t)n;
                s_last_rx_tick = xTaskGetTickCount();
            } else if (n == LIBSSH2_ERROR_EAGAIN) {
                /* No data — good time to send a keepalive if one is due.
                 * NOTE: EAGAIN can also mean "data queued but the outbound
                 * WINDOW_ADJUST would block"; the staged adjust flushes on
                 * the next wake, so treat EAGAIN strictly as end-of-wake. */
#if CONFIG_SSH_KEEPALIVE_INTERVAL > 0
                int next_ka = 0;
                int ka = libssh2_keepalive_send(s_session, &next_ka);
                if (ka < 0 && ka != LIBSSH2_ERROR_EAGAIN) {
                    log_last_error("keepalive");
                    s_connected = false;
                }
#endif
            } else if (n == 0) {
                /* Zero-length read: orderly EOF if the channel says so
                 * (remote `exit`), else just an idle cycle. NOT an error —
                 * leaving the stale libssh2 message in last_error made every
                 * clean logout look like a failure to the shell. */
                if (libssh2_channel_eof(s_channel)) {
                    ESP_LOGI(TAG, "ssh_read_task: read EOF");
                    set_last_error("");
                    s_clean_eof = true;
                    s_connected = false;
                }
            } else {
                /* Unrecoverable error */
                log_last_error("channel_read");
                s_connected = false;
            }

            /* Push out queued TX bytes (terminal replies, keystrokes the
             * UI task could not send) and any pending window-change.
             * Non-blocking, so this never parks the task holding the lock. */
            if (s_connected) {
                tx_flush();
                redraw_pump();
            }

            if (s_connected && libssh2_channel_eof(s_channel)) {
                ESP_LOGI(TAG, "ssh_read_task: channel EOF");
                set_last_error("");
                s_clean_eof = true;
                s_connected = false;
            }
            xSemaphoreGive(s_session_lock);
        } while (s_connected && n > 0 &&
                 drained < SSH_DRAIN_BUDGET &&
                 (drain_now_us() - wake_t0) < SSH_DRAIN_BUDGET_US);

        /* Present the whole batch once (held while a ?2026 synchronized
         * update is open — btop frames land atomically). Present even if the
         * session just dropped so the tail of the output reaches the display.
         * Idle wakes flush too, every ~100 ms, so the vterm ?2026 watchdog
         * can fire when the stream goes quiet mid-frame. */
        if (s_sink->flush && (drained > 0 || (++idle_wakes % 10) == 0))
            s_sink->flush(s_sink->user);

        if (!s_connected) break;

        /* net_bench accounting: gaps between data-yielding wakes. */
        if (drained > 0) {
            if (s_nb_last_data_us) {
                uint32_t gap_ms = (uint32_t)((wake_t0 - s_nb_last_data_us) / 1000);
                if (gap_ms > s_nb_gap_max_ms) s_nb_gap_max_ms = gap_ms;
                if      (gap_ms > 500) s_nb_gaps_500++;
                else if (gap_ms > 250) s_nb_gaps_250++;
            }
            s_nb_last_data_us = wake_t0;
            if (drained > s_nb_burst_max) s_nb_burst_max = (uint32_t)drained;
            s_nb_data_wakes++;
        }

        /* Periodic bench dump (arrival gaps and stalls; the consumer-side
         * duty). Each line blocks this task on the UART (~9 ms/100 chars),
         * so print on an idle wake — forced out once 5 s overdue — and skip
         * the lines that would be all zeros. */
        TickType_t now = xTaskGetTickCount();
        if ((now - last_stat) >= pdMS_TO_TICKS(30000) &&
            (drained == 0 || (now - last_stat) >= pdMS_TO_TICKS(35000))) {

            if (s_nb_data_wakes) {
                int rssi = 0;
#ifdef ESP_PLATFORM
                wifi_ap_record_t ap;
                if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
#endif
                size_t int_free = 0, int_blk = 0;
#ifdef ESP_PLATFORM
                int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                int_blk  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
#endif
                ESP_LOGI("net_bench",
                    "gap_max=%" PRIu32 "ms n250=%" PRIu32 " n500=%" PRIu32
                    " burst=%" PRIu32 "B wakes=%" PRIu32 " rssi=%d heap=%zu"
                    " int=%zu/%zu",
                    s_nb_gap_max_ms, s_nb_gaps_250, s_nb_gaps_500,
                    s_nb_burst_max, s_nb_data_wakes, rssi, s_alloc_bytes,
                    int_free, int_blk);
                net_bench_reset();     /* last_data_us survives the window */
            }

            last_stat = now;
        }

        /* Yield every wake so IDLE0 can run and reset the task WDT.
         * vTaskDelay(1) blocks for exactly one FreeRTOS tick regardless of
         * tick rate; sleep longer only when the link is idle. */
        vTaskDelay(drained > 0 ? 1 : pdMS_TO_TICKS(10));
    }

    if (s_sink->closed)
        s_sink->closed(s_clean_eof, s_sink->user);
    s_read_task_done = true;   /* disconnect() polls this before cleanup */
    s_read_task = NULL;
    vTaskDelete(NULL);
}

/* Keyboard-interactive response callback.
 * It answers every prompt with s_kb_password (set before the auth attempt).
 * libssh2 frees responses[i].text (via ssh_free) after the callback
 * returns, so this callback must allocate through ssh_malloc. Otherwise
 * the free decrements s_alloc_bytes for a block the code never counted,
 * corrupting the leak gauge.
 */
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

esp_err_t ssh_client_init(void)
{
    /* ssh_client_connect() calls libssh2_init() on the first connect. This
     * function has nothing to do here. */
    return ESP_OK;
}

/* Non-blocking connect polled with select(). Does not read errno (Winsock
 * leaves it unset): a non-zero connect() means "in progress" and the verdict
 * comes from SO_ERROR. The socket stays non-blocking, which is what libssh2
 * wants (it switches to non-blocking itself during the handshake). */
static esp_err_t tcp_connect(int sock, const struct sockaddr *addr, socklen_t len)
{
    int on = 1;
    ioctl(sock, FIONBIO, &on);
    if (s_connect_cancel) return ESP_FAIL;
    if (connect(sock, addr, len) == 0) return ESP_OK;

    const TickType_t t0 = xTaskGetTickCount();
    while (!s_connect_cancel) {
        if ((TickType_t)(xTaskGetTickCount() - t0) >= pdMS_TO_TICKS(SSH_CONNECT_TIMEOUT_MS))
            return ESP_ERR_TIMEOUT;
        fd_set wfds, efds;
        struct timeval tv = { 0, SSH_CONNECT_SLICE_MS * 1000 };
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
        FD_SET(sock, &wfds);
        FD_SET(sock, &efds);
        int n = select(sock + 1, NULL, &wfds, &efds, &tv);
        if (n < 0) return ESP_FAIL;
        if (n == 0) continue;
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0)
            return ESP_FAIL;
        return FD_ISSET(sock, &wfds) ? ESP_OK : ESP_FAIL;
    }
    return ESP_FAIL;   /* cancelled */
}

esp_err_t ssh_client_connect(const ssh_config_t *config)
{
    if (!config || !config->host || !config->username ||
        !config->sink || !config->sink->data) {
        ESP_LOGE(TAG, "Invalid config");
        return ESP_ERR_INVALID_ARG;
    }
    s_sink = config->sink;

    if (!s_session_lock) {
        s_session_lock = xSemaphoreCreateMutex();
        if (!s_session_lock) return ESP_ERR_NO_MEM;
    }

    s_fingerprint[0] = '\0';
    s_last_error[0]  = '\0';

    /* ── 0. Release any leftover state from a previous session ──
     * ssh_read_task sets s_connected=false on disconnect. It does not
     * call ssh_cleanup(). That must happen here before the code
     * allocates new libssh2 objects, or session_init() fails with OOM. */
    ssh_client_disconnect();
    s_clean_eof = false;

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
    esp_err_t crc = tcp_connect(s_sock, res->ai_addr, (socklen_t)res->ai_addrlen);
    freeaddrinfo(res);
    if (crc != ESP_OK) {
        ESP_LOGE(TAG, "TCP connect %s",
                 crc == ESP_ERR_TIMEOUT ? "timed out" :
                 s_connect_cancel ? "cancelled" : "failed");
        set_last_error(crc == ESP_ERR_TIMEOUT ? "Connection timed out" :
                       s_connect_cancel ? "Cancelled" : "TCP connect failed");
        ssh_cleanup();
        return ESP_FAIL;
    }
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
     * land on stderr, which goes to the UART console. This call is a
     * no-op stub unless the libssh2_esp component enables
     * CONFIG_LIBSSH2_DEBUG_ENABLE at build time. */
    libssh2_trace(s_session, LIBSSH2_TRACE_ERROR | LIBSSH2_TRACE_AUTH |
                             LIBSSH2_TRACE_PUBLICKEY);
    /* Never let the blocking handshake/auth hang forever on a dead link
     * (do_connect runs synchronously on the shell task). */
    libssh2_session_set_timeout(s_session, SSH_BLOCKING_TIMEOUT_MS);

    ESP_LOGI(TAG, "SSH handshake...");
    int rc = libssh2_session_handshake(s_session, s_sock);
    if (rc != 0) {
        ESP_LOGE(TAG, "SSH handshake failed: %d", rc);
        log_last_error("handshake");
        ssh_cleanup();
        return ESP_FAIL;
    }

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
        /* Key auth fails far more often from a mis-stored key or a missing
         * passphrase than from anything libssh2 can name. This code dumps
         * what it fed the auth call only on the failure path, so a good
         * connect stays quiet. The BEGIN line is public. The key body
         * never reaches the console. This code reports the passphrase by
         * length alone. */
        {
            const char *pem = config->private_key_pem;
            size_t hdr = strcspn(pem, "\r\n");
            if (hdr > 48) hdr = 48;
            ESP_LOGE(TAG, "  key header: \"%.*s\"", (int)hdr, pem);
            ESP_LOGE(TAG, "  passphrase: %s (%u chars), pubkey: %s",
                     config->passphrase ? "yes" : "NONE",
                     (unsigned)(config->passphrase ? strlen(config->passphrase) : 0),
                     config->public_key_pem ? "yes" : "no");
        }
        ssh_cleanup();
        return SSH_ERR_AUTH;
    }

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

#if CONFIG_SSH_KEEPALIVE_INTERVAL > 0
    /* Keepalive starts only after userauth: some servers drop a global
     * request that arrives mid-auth. want_reply=1 lets the read task
     * detect a silent peer. See docs/ARCHITECTURE.md, "SSH uplink and
     * liveness". */
    libssh2_keepalive_config(s_session, 1 /* want_reply */,
                             CONFIG_SSH_KEEPALIVE_INTERVAL);
    ESP_LOGI(TAG, "Keepalive every %d s", CONFIG_SSH_KEEPALIVE_INTERVAL);
#endif

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

    /* PTY size comes with the config; the terminal-free default is the
     * classic 80x24. */
    int term_cols = config->term_cols ? config->term_cols : 80;
    int term_rows = config->term_rows ? config->term_rows : 24;
    s_term_cols = (uint16_t)term_cols;
    s_term_rows = (uint16_t)term_rows;
    rc = libssh2_channel_request_pty_ex(s_channel,
                                        "xterm-256color", 14,
                                        NULL, 0,
                                        term_cols,
                                        term_rows,
                                        0, 0);
    if (rc != 0) {
        ESP_LOGE(TAG, "PTY request failed: %d", rc);
        ssh_cleanup();
        return ESP_FAIL;
    }

    rc = libssh2_channel_shell(s_channel);
    if (rc != 0) {
        ESP_LOGE(TAG, "Shell request failed: %d", rc);
        ssh_cleanup();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Shell opened");

    /* ── 10. Switch to non-blocking for the read loop ───────────────── */
    libssh2_session_set_blocking(s_session, 0);

        /* The session controller reset the terminal before this connect
     * began. The read task below is the session's sole consumer-side
     * writer. */
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
    /* This forces full radio wakefulness for the session. By default,
     * modem-sleep gates receive on the AP's DTIM beacon (tens of ms RTT).
     * That caps window-bound throughput and every key echo. Disconnect
     * restores the previous power-save mode. With the BLE keyboard
     * connected, coexistence still time-slices the radio, so this removes
     * only the DTIM-gated part of the latency. */
    esp_wifi_set_ps(WIFI_PS_NONE);
#endif

    ESP_LOGI(TAG, "SSH session ready — libssh2 heap: %zu B", s_alloc_bytes);
    return ESP_OK;
}

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

    s_connect_cancel = false;       /* before the worker can see a Cancel */
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
    /* The flag ends a pending TCP connect; shutdown() unblocks later phases
     * (ssh_cleanup() owns the close). DNS resolution is not interruptible. */
    s_connect_cancel = true;
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
        /* This should never happen now that the read task holds the lock
         * only across non-blocking calls. If it does happen, the killed
         * task may still own s_session_lock; FreeRTOS never releases a
         * deleted task's mutex. So this code recreates the lock, or every
         * later take() would deadlock. This is safe here. Disconnect runs
         * on the shell task, the only other taker. So nobody holds the
         * lock at this instant. */
        ESP_LOGE(TAG, "ssh_read_task did not exit — force-deleting");
        vTaskDelete(s_read_task);
        s_read_task = NULL;
        s_read_task_done = true;
        vSemaphoreDelete(s_session_lock);
        s_session_lock = xSemaphoreCreateMutex();
    }

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

    /* UI task: append, try one immediate write, return. Whatever the link
     * cannot take now stays queued and the read task flushes it. */
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    size_t took = s_channel ? ssh_txbuf_append(&s_tx, data, len) : 0;
    if (took) tx_flush();
    xSemaphoreGive(s_session_lock);

    return took ? (int)took : -1;
}

int ssh_client_request_redraw(void)
{
    if (!s_connected || !s_channel) return -1;
    /* sshd raises SIGWINCH only on a size change. So: one column narrower,
     * then the real size. The read task completes both steps. A redraw in
     * flight keeps its place. */
    xSemaphoreTake(s_session_lock, portMAX_DELAY);
    if (s_channel && s_redraw_step == 0) s_redraw_step = 1;
    redraw_pump();
    xSemaphoreGive(s_session_lock);
    return 0;
}

bool ssh_client_is_connected(void)
{
    return s_connected;
}

bool ssh_client_session_eof(void)
{
    return s_clean_eof;
}

const char *ssh_client_get_fingerprint(void)
{
    return s_fingerprint;
}

const char *ssh_client_last_error(void)
{
    return s_last_error;
}
