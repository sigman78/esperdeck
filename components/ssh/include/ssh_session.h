/*
 * ssh_session.h — the session controller (extensibility item 7).
 *
 * The policy half of the ssh component. It arms and retries connect
 * attempts, resolves key material, and stops on hostkey decisions.
 * It also wires the transport's byte sink to the terminal. The screen
 * stays thin. It renders controller state and routes events to navigation.
 * The screen drives the controller with one ssh_session_poll() per
 * tick.
 */

#pragma once

#include "esp_err.h"
#include "storage.h"     /* conn_profile_t */
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    SSH_SESSION_IDLE = 0,
    SSH_SESSION_ARMED,          /* waiting out not_before        */
    SSH_SESSION_CONNECTING,     /* async worker in flight        */
    SSH_SESSION_CANCELLING,     /* cancel sent, worker finishing */
    SSH_SESSION_UP,             /* live shell                    */
} ssh_session_state_t;

/* One-shot outcomes. ssh_session_poll() returns each exactly once;
 * the screen turns them into toasts and navigation. */
typedef enum {
    SSH_SESSION_EV_NONE = 0,
    SSH_SESSION_EV_CONNECTED,
    SSH_SESSION_EV_NEED_UNLOCK,      /* key profile, store locked   */
    SSH_SESSION_EV_KEY_UNREADABLE,
    SSH_SESSION_EV_HOSTKEY_UNKNOWN,  /* TOFU prompt; fp available   */
    SSH_SESSION_EV_HOSTKEY_MISMATCH,
    SSH_SESSION_EV_AUTH_FAILED,
    SSH_SESSION_EV_BUSY,             /* a connect already in flight */
    SSH_SESSION_EV_RETRYING,         /* connect failed; re-armed    */
    SSH_SESSION_EV_DROP_RETRYING,    /* session dropped; re-armed   */
    SSH_SESSION_EV_DROPPED,          /* ended; clean via _eof()     */
    SSH_SESSION_EV_FAILED,           /* terminal failure, no retry  */
    SSH_SESSION_EV_CANCELLED,
} ssh_session_event_t;

typedef struct {
    uint32_t retry_delay_ms;         /* auto-reconnect backoff      */
    bool     auto_reconnect;
    int      term_cols, term_rows;   /* PTY geometry for the remote */
} ssh_session_policy_t;

/* Snapshot @p profile and arm a connect for not_before. The pinned
 * host fingerprint loads from storage here. */
void ssh_session_begin(const conn_profile_t *profile,
                       const ssh_session_policy_t *policy,
                       uint64_t not_before, uint64_t now);

/* Re-arm the existing snapshot (unlock-screen resume). */
void ssh_session_rearm(uint64_t now);

/* Re-arm with @p fp pre-pinned — the hostkey prompt's trust path. */
void ssh_session_rearm_pinned(const char *fp, uint64_t now);

/* Abort. An armed attempt cancels at once. An in-flight worker gets a
 * best-effort unblock and reports through EV_CANCELLED. */
void ssh_session_cancel(uint64_t now);

/* Advance the state machine; call once per screen tick. */
ssh_session_event_t ssh_session_poll(uint64_t now);

ssh_session_state_t   ssh_session_state(void);
const conn_profile_t *ssh_session_profile(void);   /* the snapshot     */
uint64_t ssh_session_retry_at(void);               /* countdown target */
uint64_t ssh_session_attempt_started(void);        /* elapsed display  */
int      ssh_session_attempt(void);                /* 0 = user-started */

/* Wipe the snapshot's secret (the app_creds_wipe companion). */
void ssh_session_creds_wipe(void);
