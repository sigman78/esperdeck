/*
 * ssh_session.c — the session controller (extensibility item 7).
 *
 * Policy over the terminal-free transport. It owns arm/retry
 * scheduling, key and secret resolution, hostkey stops, and the vterm
 * wiring (sink + reply queue). One poll per screen tick drives
 * everything. UI — toasts, navigation, prompts — stays in the screen.
 */

#include "ssh_session.h"
#include "ssh_client.h"
#include "vterm.h"
#include "keystore.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "ssh_session";

static struct {
    conn_profile_t       active;    /* connect/session snapshot        */
    ssh_session_policy_t pol;
    char     pinned_fp[65];         /* expected_fp, "" = none          */
    uint64_t connect_at;            /* not before                      */
    uint64_t attempt_started;
    int      attempt;               /* counts auto-retries             */
    uint8_t  state;                 /* ssh_session_state_t             */
    uint8_t  pending;               /* ssh_session_event_t, one slot   */
} s_ss;

/* The shell task reads key PEMs, not the connect worker. The worker's
 * stack lives in PSRAM and must not touch littlefs, since flash I/O
 * asserts there. The PEMs must outlive the async connect: the worker
 * reads the cfg strings during the handshake. The PRIVATE key buffer
 * lives in INTERNAL SRAM, never PSRAM, since the external bus is
 * probeable (docs/storage_auth.md RAM hygiene). This controller wipes
 * it the moment the handshake finishes. The .pub buffer stays public. */
enum { KEY_PEM_MAX = 8192, PUB_PEM_MAX = 2048 };
static char *s_key_pem = NULL;
static char *s_pub_pem = NULL;

/* Connect-time credential (password or key passphrase). The profile
 * snapshot may predate the unlock (lazy gate) and so miss its diverted
 * secret — resolve from the bundle here, .bss (internal SRAM), wiped
 * with the key PEM once the handshake is over. */
static char s_secret[sizeof(((conn_profile_t *)0)->password)];

static const char *resolve_secret(const conn_profile_t *p)
{
    /* A real value in the snapshot wins; the @bundle marker (a snapshot
     * taken while locked) must NEVER reach libssh2 as a literal. */
    if (p->password[0] && strcmp(p->password, STORAGE_PW_BUNDLED) != 0)
        return p->password;
    if (keystore_state() == KEYSTORE_UNLOCKED) {
        char skey[48];
        snprintf(skey, sizeof(skey), "profile:%s", p->name);
        if (keystore_secret_get(skey, s_secret, sizeof(s_secret)) == ESP_OK)
            return s_secret;
    }
    return "";
}

static void wipe_key_pem(void)
{
    if (s_key_pem) keystore_wipe(s_key_pem, KEY_PEM_MAX);
    keystore_wipe(s_secret, sizeof(s_secret));
}

/* ---- the terminal wiring (the one vterm consumer in this component) */

static void sink_data(const char *buf, size_t len, void *user)
{
    (void)user;
    vterm_feed(buf, len);               /* parse only; present on flush */
}

static void sink_flush(void *user)
{
    (void)user;
    vterm_flush();

    /* vterm parse-split bench, 30 s cadence (rode the read task before
     * the split). Flush only runs amid traffic, which is when the
     * numbers mean anything. */
    static TickType_t s_last;
    TickType_t now = xTaskGetTickCount();
    if ((now - s_last) >= pdMS_TO_TICKS(30000)) {
        s_last = now;
        vterm_bench_report();
        vterm_bench_reset();
    }
}

static void sink_closed(bool clean_eof, void *user)
{
    (void)clean_eof; (void)user;        /* the screen polls state */
}

static const ssh_sink_t s_sink = {
    .data = sink_data, .flush = sink_flush, .closed = sink_closed,
};

static void response_cb(const char *data, size_t len, void *user)
{
    (void)user;
    ssh_client_queue_reply((const uint8_t *)data, len);
}

static void set_pending(ssh_session_event_t ev)
{
    s_ss.pending = (uint8_t)ev;
}

/* Resolve credentials and launch the async connect (shell task: flash
 * I/O is safe here, the worker's PSRAM stack is not). */
static void launch(uint64_t now)
{
    const conn_profile_t *p = &s_ss.active;

    ssh_config_t cfg = {
        .host        = p->host,
        .port        = p->port,
        .username    = p->user,
        .expected_fp = s_ss.pinned_fp[0] ? s_ss.pinned_fp : NULL,
        .sink        = &s_sink,
        .term_cols   = (uint16_t)s_ss.pol.term_cols,
        .term_rows   = (uint16_t)s_ss.pol.term_rows,
    };
    if (p->auth == STORAGE_AUTH_KEY) {
        /* A locked store can slip past the screen's gate (reconnect,
         * re-arm, or a lock trigger fired since): bounce to unlock. */
        if (keystore_state() == KEYSTORE_LOCKED) {
            s_ss.state = SSH_SESSION_IDLE;
            set_pending(SSH_SESSION_EV_NEED_UNLOCK);
            return;
        }
        if (!s_key_pem) s_key_pem = heap_caps_malloc(KEY_PEM_MAX,
                                        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_pub_pem) s_pub_pem = heap_caps_malloc(PUB_PEM_MAX,
                                        MALLOC_CAP_SPIRAM);
        size_t klen = 0;
        esp_err_t ge = (!s_key_pem || !s_pub_pem) ? ESP_ERR_NO_MEM
                     : storage_get_key(p->key_id, s_key_pem, KEY_PEM_MAX, &klen);
        if (ge == ESP_ERR_INVALID_STATE) {
            s_ss.state = SSH_SESSION_IDLE;
            set_pending(SSH_SESSION_EV_NEED_UNLOCK);
            return;
        }
        if (ge != ESP_OK) {
            ESP_LOGE(TAG, "key '%s' unreadable", p->key_id);
            s_ss.state = SSH_SESSION_IDLE;
            set_pending(SSH_SESSION_EV_KEY_UNREADABLE);
            return;
        }
        cfg.private_key_pem = s_key_pem;
        const char *pw = resolve_secret(p);      /* key passphrase */
        cfg.passphrase = pw[0] ? pw : NULL;

        char pub_path[160];   /* optional .pub beside the key */
        snprintf(pub_path, sizeof(pub_path), "%s/keys/%s.pub",
                 storage_platform_mount_point(), p->key_id);
        FILE *pf = fopen(pub_path, "r");
        if (pf) {
            size_t n = fread(s_pub_pem, 1, PUB_PEM_MAX - 1, pf);
            fclose(pf);
            if (n > 0) { s_pub_pem[n] = '\0'; cfg.public_key_pem = s_pub_pem; }
        }
    } else {
        cfg.password = resolve_secret(p);
    }

    /* The read task is not up yet and the worker never touches vterm.
     * The reset below cannot race a feed. A fresh session must not
     * inherit SGR colors, the alt screen, DECCKM, charsets, or the old
     * host's scrollback. That takes a full reset, not an ED clear —
     * ED fills with the CURRENT colors. */
    vterm_reset();
    vterm_bench_reset();
    vterm_set_response_cb(response_cb, NULL);

    if (ssh_client_connect_start(&cfg) != ESP_OK) {
        wipe_key_pem();
        s_ss.state = SSH_SESSION_IDLE;
        set_pending(SSH_SESSION_EV_BUSY);
        return;
    }
    s_ss.state           = SSH_SESSION_CONNECTING;
    s_ss.attempt_started = now;
}

/* Classify the async connect result. */
static ssh_session_event_t finish(uint64_t now, esp_err_t err)
{
    /* Handshake over either way — libssh2 has derived what it needs, so
     * the plaintext key must not linger for the session's lifetime. */
    wipe_key_pem();
    if (s_ss.state == SSH_SESSION_CANCELLING) {
        if (err == ESP_OK)
            ssh_client_disconnect();   /* it connected as we cancelled */
        s_ss.state = SSH_SESSION_IDLE;
        return SSH_SESSION_EV_CANCELLED;
    }
    switch (err) {
    case ESP_OK:
        s_ss.state = SSH_SESSION_UP;
        return SSH_SESSION_EV_CONNECTED;
    case SSH_ERR_HOSTKEY_UNKNOWN:
        s_ss.state = SSH_SESSION_IDLE;
        return SSH_SESSION_EV_HOSTKEY_UNKNOWN;
    case SSH_ERR_HOSTKEY_MISMATCH:
        s_ss.state = SSH_SESSION_IDLE;
        return SSH_SESSION_EV_HOSTKEY_MISMATCH;
    case SSH_ERR_AUTH:
        s_ss.state = SSH_SESSION_IDLE;
        return SSH_SESSION_EV_AUTH_FAILED;
    default:
        if (s_ss.pol.auto_reconnect) {
            s_ss.attempt++;
            s_ss.connect_at = now + s_ss.pol.retry_delay_ms;
            s_ss.state = SSH_SESSION_ARMED;
            return SSH_SESSION_EV_RETRYING;
        }
        s_ss.state = SSH_SESSION_IDLE;
        return SSH_SESSION_EV_FAILED;
    }
}

static void load_pinned_fp(void)
{
    if (storage_known_host_get(s_ss.active.host, s_ss.active.port,
                               s_ss.pinned_fp, sizeof(s_ss.pinned_fp)) != ESP_OK)
        s_ss.pinned_fp[0] = '\0';
}

void ssh_session_begin(const conn_profile_t *profile,
                       const ssh_session_policy_t *policy,
                       uint64_t not_before, uint64_t now)
{
    (void)now;
    s_ss.active  = *profile;
    s_ss.pol     = *policy;
    s_ss.attempt = 0;
    load_pinned_fp();
    s_ss.connect_at = not_before;
    s_ss.state      = SSH_SESSION_ARMED;
}

void ssh_session_rearm(uint64_t now)
{
    s_ss.attempt = 0;
    load_pinned_fp();
    s_ss.connect_at = now;
    s_ss.state      = SSH_SESSION_ARMED;
}

void ssh_session_rearm_pinned(const char *fp, uint64_t now)
{
    snprintf(s_ss.pinned_fp, sizeof(s_ss.pinned_fp), "%s", fp);
    s_ss.attempt    = 0;
    s_ss.connect_at = now;
    s_ss.state      = SSH_SESSION_ARMED;
}

void ssh_session_cancel(uint64_t now)
{
    (void)now;
    if (s_ss.state == SSH_SESSION_ARMED) {
        s_ss.state = SSH_SESSION_IDLE;
        set_pending(SSH_SESSION_EV_CANCELLED);
    } else if (s_ss.state == SSH_SESSION_CONNECTING) {
        s_ss.state = SSH_SESSION_CANCELLING;
        ssh_client_connect_cancel();        /* best-effort unblock */
    }
}

ssh_session_event_t ssh_session_poll(uint64_t now)
{
    if (s_ss.pending) {
        ssh_session_event_t ev = (ssh_session_event_t)s_ss.pending;
        s_ss.pending = 0;
        return ev;
    }
    switch (s_ss.state) {
    case SSH_SESSION_ARMED:
        if (now >= s_ss.connect_at) {
            launch(now);                     /* may set a pending event */
            if (s_ss.pending) {
                ssh_session_event_t ev = (ssh_session_event_t)s_ss.pending;
                s_ss.pending = 0;
                return ev;
            }
        }
        break;
    case SSH_SESSION_CONNECTING:
    case SSH_SESSION_CANCELLING:
        if (ssh_client_connect_ready())
            return finish(now, ssh_client_connect_take_result());
        break;
    case SSH_SESSION_UP:
        if (!ssh_client_is_connected()) {
            if (!ssh_client_session_eof() && s_ss.pol.auto_reconnect) {
                s_ss.attempt++;
                s_ss.connect_at = now + s_ss.pol.retry_delay_ms;
                s_ss.state = SSH_SESSION_ARMED;
                return SSH_SESSION_EV_DROP_RETRYING;
            }
            s_ss.state = SSH_SESSION_IDLE;
            return SSH_SESSION_EV_DROPPED;
        }
        break;
    default:
        break;
    }
    return SSH_SESSION_EV_NONE;
}

ssh_session_state_t ssh_session_state(void)
{
    return (ssh_session_state_t)s_ss.state;
}

const conn_profile_t *ssh_session_profile(void)  { return &s_ss.active; }
uint64_t ssh_session_retry_at(void)              { return s_ss.connect_at; }
uint64_t ssh_session_attempt_started(void)       { return s_ss.attempt_started; }
int      ssh_session_attempt(void)               { return s_ss.attempt; }

void ssh_session_creds_wipe(void)
{
    keystore_wipe(s_ss.active.password, sizeof(s_ss.active.password));
}
