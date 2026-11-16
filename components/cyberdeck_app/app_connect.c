/*
 * app_connect.c — the thin connect/session screens. CONNECTING renders
 * the controller state (armed, worker, retry countdown). After a failed
 * connect it shows a page with the reason and three ways on. SESSION
 * owns the live chrome (toast box, scrollback bar) and input. Connect
 * policy lives in ssh_session.c: retry, key resolution, hostkey stops
 * (extensibility item 7).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "display.h"     /* display_get_text_size — the PTY geometry */
#include "display_fx.h"
#include "font.h"        /* font_height() — drag pixels to terminal rows */
#include "keystore.h"
#include "session_guard.h"
#include "ssh_client.h"
#include "ssh_session.h"
#include "ui_pure.h"     /* line wrap, UTF-8 put */
#include "vterm.h"
#include "vtkeys.h"      /* the session encodes HIDKEY events at send */
#include "wifi_manager.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

#ifdef CONFIG_DISPLAY_ISR_BENCH
#include <inttypes.h>
#endif

static uint64_t s_session_start;    /* session_enter() time, NO CARRIER */

const conn_profile_t *conn_active(void)   { return ssh_session_profile(); }
uint64_t conn_session_start(void)         { return s_session_start; }

void conn_creds_wipe(void)
{
    ssh_session_creds_wipe();
}

/* The controller policy is app config plus the display grid. */
static ssh_session_policy_t conn_policy(void)
{
    ssh_session_policy_t pol = {
        .retry_delay_ms = app.cfg.ssh_retry_delay_ms,
        .auto_reconnect = app.cfg.auto_reconnect,
    };
    display_get_text_size(&pol.term_cols, &pol.term_rows);
    if (pol.term_cols <= 0 || pol.term_rows <= 0) {
        pol.term_cols = display_text_cols();
        pol.term_rows = display_text_rows();
    }
    return pol;
}

enum { ID_CANCEL = 1, ID_BACK, ID_AGAIN, ID_EDIT };

static const char *TAG = "app_connect";

/* The failed view lives on this screen. It keeps its own copy of the
 * reason: the client's error buffer changes with the next attempt. */
static struct {
    bool failed;
    bool idle_cancel;     /* the idle lock cancelled the attempt */
    char reason[128];
    char name[sizeof(((conn_profile_t *)0)->name)];
} s_conn;

/* The stored host of that name, or -1. Retry and edit work on it. */
static int stored_host(const char *name)
{
    for (int i = 0; i < app.stored_count; i++)
        if (strcmp(app.profiles[i].name, name) == 0) return i;
    return -1;
}

/* "Next try in 3 s" counts up to the whole second. */
static void live_line(char *out, size_t sz, ssh_session_state_t st,
                      uint64_t now)
{
    out[0] = '\0';
    if (st == SSH_SESSION_ARMED && ssh_session_retry_at() > now) {
        snprintf(out, sz, "Next try in %u s",
                 (unsigned)((ssh_session_retry_at() - now + 999) / 1000));
    } else if (st == SSH_SESSION_CONNECTING) {
        const unsigned s = now > ssh_session_attempt_started()
            ? (unsigned)((now - ssh_session_attempt_started()) / 1000) : 0;
        if (ssh_session_attempt() > 0)
            snprintf(out, sz, "Try %d, %u s", ssh_session_attempt() + 1, s);
        else
            snprintf(out, sz, "%u s", s);
    }
}

static void render_failed(ui_slate_t *pg, const conn_profile_t *p)
{
    char head[64], addr[160], l1[128], l2[128];

    snprintf(addr, sizeof addr, "%s@%s port %u", p->user, p->host,
             (unsigned)p->port);
    snprintf(head, sizeof head, "No session on %s", s_conn.name);
    /* The value column: the panel less its margins and the label column. */
    const int nl = ui_wrap2(s_conn.reason, 46 - 4 - 3, l1, l2, sizeof l1);
    ui_info_row_t rows[5] = {
        { .label = UI_S_CROSS, .value = head, .value_pen = OVERLAY_COL_WHITE,
          .label_pen = OVERLAY_COL_RED },
        { .label = "", .value = "" },
        { .label = "", .value = l1 },
    };
    int n = 3;
    if (nl == 2) rows[n++] = (ui_info_row_t){ .label = "", .value = l2 };
    rows[n++] = (ui_info_row_t){ .label = "", .value = addr };

    ui_rect_t box = ui_center(pg->area, 46, n + 5);
    const ui_rect_t card = ui_cut_top(&box, n + 2);
    ui_info(card, rows, n, OVERLAY_COL_RED);

    const bool stored = stored_host(s_conn.name) >= 0;
    ui_focus_default(stored ? ID_AGAIN : ID_BACK);
    ui_flow_t f = ui_flow(box, pg->m.gutter, 3);
    ui_push(ui_flow_next(&f, ui_push_w("Back")), ID_BACK, "Back",
            UI_TONE_CARD, 0, 0);
    if (stored) {
        ui_push(ui_flow_next(&f, ui_push_w("Try again")), ID_AGAIN,
                "Try again", UI_TONE_CARD, 0, 0);
        ui_push(ui_flow_next(&f, ui_push_w("Edit host")), ID_EDIT,
                "Edit host", UI_TONE_SOLID, OVERLAY_COL_BLUE, 0);
    }
}

static void render_connecting(uint64_t now)
{
    const int faults = ui_faults();
    ui_slate_t pg = ui_slate_page();
    const conn_profile_t *p = ssh_session_profile();

    if (s_conn.failed) {
        render_failed(&pg, p);
    } else {
        /* The status word falls out of the controller state. */
        const ssh_session_state_t st = ssh_session_state();
        const bool cancelling = st == SSH_SESSION_CANCELLING;
        const bool retry = ssh_session_attempt() > 0 ||
            (st == SSH_SESSION_ARMED && now < ssh_session_retry_at());
        char spin[8], head[64], wifi[64], addr[160], live[40];

        ui_utf8_put(spin, spinner_glyph(app.anim_frame));
        if (cancelling) snprintf(head, sizeof head, "Cancelling");
        else snprintf(head, sizeof head, "%s %s",
                      retry ? "Retrying" : "Connecting to", p->name);
        const bool joined = wifi_manager_get_state() == WIFI_MGR_CONNECTED;
        snprintf(wifi, sizeof wifi, "Wi-Fi  %s",
                 joined ? wifi_manager_get_ssid() : wifi_status_str());
        snprintf(addr, sizeof addr, "%s@%s port %u", p->user, p->host,
                 (unsigned)p->port);
        live_line(live, sizeof live, st, now);

        const ui_info_row_t rows[5] = {
            { .label = spin, .value = head, .label_pen = OVERLAY_COL_AMBER,
              .value_pen = cancelling ? OVERLAY_COL_AMBER : OVERLAY_COL_WHITE },
            { .label = "", .value = "" },
            { .label = joined ? UI_S_CHECK : UI_S_CROSS, .value = wifi,
              .label_pen = joined ? OVERLAY_COL_GREEN : OVERLAY_COL_RED },
            { .label = "", .value = addr },
            { .label = "", .value = live },
        };
        ui_rect_t box = ui_center(pg.area, 46, 10);
        const ui_rect_t card = ui_cut_top(&box, 7);
        ui_info(card, rows, 5, prof_accent(p->name));
        ui_focus_default(ID_CANCEL);
        ui_push(ui_rect(box.x, box.y, ui_push_w("Cancel"), 3), ID_CANCEL,
                "Cancel", UI_TONE_CARD, 0, 0);
    }
    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

#define SCROLLBAR_LINGER_MS  1400

static uint64_t s_scrollbar_until;   /* 0 = not showing */

/* Drag travel not yet spent (shared accumulate-then-floor converter). */
static ui_drag_t s_scroll_drag;

void session_scroll_seen(uint64_t now)
{
    s_scrollbar_until = now + SCROLLBAR_LINGER_MS;
}

/* Toast and scrollback indicator share the overlay, so one clear/present
 * pass draws both. */
static void render_session_chrome(uint64_t now)
{
    const bool toast_on = now < app.toast_until && app.toast[0];
    const bool bar_on   = s_scrollbar_until && now < s_scrollbar_until &&
                          vterm_scroll_len() > 0;
    if (!s_scrollbar_until || now >= s_scrollbar_until) s_scrollbar_until = 0;

    if (!toast_on && !bar_on) {
        if (nav_current() == SCR_SESSION) ui_hide();
        return;
    }

    ui_colors(UI_FG, UI_BG);
    ui_clear();

    if (bar_on)
        draw_scrollbar(vterm_scroll_offset(), vterm_scroll_len());

    if (!toast_on) {
        ui_present();
        return;
    }

    /* The status row's note look: a plain panel-tone box. */
    ui_toast(ui_rect(0, 0, ui_cols() - 1, 1), app.toast,
             app.toast_ok ? UI_NOTE_GOOD : UI_NOTE_PLAIN);
    ui_present();
}

/* Arm a connect screen-side: the controller is already armed. The
 * connect takes the whole stack, wherever it started. Every way out
 * lands on HOME. The session's F12 then finds no stale SCR_MENU. */
static void show_connecting(uint64_t now)
{
    if (nav_current() != SCR_CONNECTING) {
        nav_reset(SCR_CONNECTING, 0, now);
        return;
    }
    if (s_conn.failed) {                  /* the failed view gives way */
        s_conn.failed = false;
        ui_hot_reset();
    }
    s_conn.idle_cancel = false;
    nav_invalidate();
}

/* Arm a connect to profile @p idx, snapshotting it into the controller. */
void start_connect(int idx, uint64_t not_before, uint64_t now)
{
    const conn_profile_t *p = &app.profiles[idx];
    /* A key profile with a locked store must unlock first. On success,
     * the PIN pad re-arms this connect (the lazy on-first-key-use
     * trigger). This gate holds even when this key is still a bare
     * .pem. A present store means the store already protects the keys,
     * or unlock will adopt them. Plaintext must never silently bypass
     * the lock. An absent store turns the feature off. */
    if (p->auth == STORAGE_AUTH_KEY &&
        keystore_state() == KEYSTORE_LOCKED) {
        ssh_session_policy_t pol = conn_policy();
        ssh_session_begin(p, &pol, not_before, now);
        unlock_open(now, true);
        return;
    }
    ssh_session_policy_t pol = conn_policy();
    ssh_session_begin(p, &pol, not_before, now);
    show_connecting(now);
}

/* Re-arm a connect to the ACTIVE snapshot (unlock-screen resume). */
void connect_resume_active(uint64_t now)
{
    ssh_session_rearm(now);
    show_connecting(now);
}

/* Re-arm a connect to the active snapshot with @p fp pre-pinned — the
 * hostkey prompt's trust path (explicit user action, not a retry). */
void connect_arm_pinned(const char *fp, uint64_t now)
{
    ssh_session_rearm_pinned(fp, now);
    show_connecting(now);
}

/* The session died, and auto-reconnect is off: the classic modem death
 * rattle. It shows link time and the drop reason ("" on a clean EOF). */
void session_dropped(uint64_t now)
{
    uint32_t dur = (uint32_t)((now - s_session_start) / 1000);
    const char *why = ssh_client_last_error();
    display_bell();
    if (why[0]) display_fx_static();   /* transport error: signal-loss snow */
    if (why[0])
        toast_for(now, ERR_TOAST_MS, "NO CARRIER (%02u:%02u) - %.36s",
                  dur / 60, dur % 60, why);
    else
        toast(now, "NO CARRIER (%02u:%02u)", dur / 60, dur % 60);
    enter_home_after_collapse(now);    /* CRT power-off over the dead screen */
}

/* Hand the screen back to a live session (nav resume: menu/pairing pop).
 * Every full-screen modal parks the terminal cursor (ui_no_cursor) and a
 * vterm flush only happens when the host sends bytes — without the
 * explicit refresh the cursor stays gone until the first input. */
static void session_resume(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    ui_hide();
    vterm_cursor_refresh();
}

static void session_enter(intptr_t arg, uint64_t now)
{
    (void)arg;
    /* The display now belongs to remote content: blank every sprite slot so
     * terminal text in U+E000.. (e.g. Nerd-Font icons) renders blank cells,
     * not leftover HOME marquee art (font.h: local UI namespace). */
    font_sprite_clear_all();

    s_session_start = now;
#ifdef CONFIG_DISPLAY_ISR_BENCH
    display_render_bench_reset();
#endif
    display_fx_wipe();     /* raster-reveal the fresh session */
    session_resume(0, now);
    /* ssh_client_connect() clears the terminal before the read task
     * spawns. Clearing it here would race that task inside vterm. */
    static const char *const HELLO[] = {
        "jacked in", "link up", "uplink established",
        "handshake clean", "you're in",
    };
    toast(now, "%s - F12 or long-press for menu",
          HELLO[app.anim_frame % (sizeof(HELLO) / sizeof(HELLO[0]))]);
    app.toast_ok = true;       /* the toast shows a check mark */
    render_session_chrome(now);
}

/* Open the failed view. The snapshot's password goes first: the page
 * can stay open. Try again re-reads the stored host instead. The view
 * opens a fresh idle window: a failure after a quiet session stays up.
 * Once the deck idles, the page locks the store. */
static void conn_failed(const char *reason, uint64_t now)
{
    conn_creds_wipe();
    session_guard_activity(now);
    snprintf(s_conn.reason, sizeof s_conn.reason, "%s", reason);
    snprintf(s_conn.name, sizeof s_conn.name, "%s",
             ssh_session_profile()->name);
    s_conn.failed = true;
    ui_hot_reset();
    nav_invalidate();
}

/* Route a controller event to navigation and toasts. True when the
 * event navigated away (the caller stops its tick). */
static bool handle_session_event(ssh_session_event_t ev, uint64_t now)
{
    char why[128];
    const char *err = ssh_client_last_error();

    switch (ev) {
    case SSH_SESSION_EV_CONNECTED:
        nav_replace(SCR_SESSION, 0, now);
        return true;
    case SSH_SESSION_EV_NEED_UNLOCK:
        unlock_open(now, true);
        return true;
    case SSH_SESSION_EV_KEY_UNREADABLE:
        snprintf(why, sizeof why, "Key '%.31s' cannot be read",
                 ssh_session_profile()->key_id);
        conn_failed(why, now);
        return true;
    case SSH_SESSION_EV_HOSTKEY_UNKNOWN:
        hostkey_open(false, now);
        return true;
    case SSH_SESSION_EV_HOSTKEY_MISMATCH:
        hostkey_open(true, now);
        return true;
    case SSH_SESSION_EV_AUTH_FAILED:
        if (err[0]) snprintf(why, sizeof why, "Sign in refused: %.100s", err);
        else snprintf(why, sizeof why, "Sign in refused");
        conn_failed(why, now);
        return true;
    case SSH_SESSION_EV_BUSY:
        conn_failed("Another connect is still running", now);
        return true;
    case SSH_SESSION_EV_RETRYING:
        toast(now, "connect failed - retrying");
        return false;                    /* countdown renders here */
    case SSH_SESSION_EV_FAILED:
        conn_failed(err[0] ? err : "The connection failed", now);
        return true;
    case SSH_SESSION_EV_CANCELLED:
        if (s_conn.idle_cancel && session_guard_lock_gate(now)) return true;
        toast(now, "cancelled");
        enter_home(now);
        return true;
    default:
        return false;
    }
}

static void connecting_tick(uint64_t now)
{
    if (handle_session_event(ssh_session_poll(now), now))
        return;
    /* The idle lock skips this page (session_guard.c). A retry loop runs
     * forever, so an idle attempt stops first. The lock follows on
     * EV_CANCELLED. */
    if (session_guard_idle(now)) {
        if (s_conn.failed) {
            if (!session_guard_lock_gate(now)) enter_home(now);
            return;
        }
        if (!s_conn.idle_cancel && keystore_state() == KEYSTORE_UNLOCKED) {
            s_conn.idle_cancel = true;
            ssh_session_cancel(now);
        }
    }
    if (s_conn.failed) return;           /* a still page */
    if (now >= app.next_anim) {          /* keep the spinner/counters alive */
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

static void session_tick(uint64_t now)
{
    switch (ssh_session_poll(now)) {
    case SSH_SESSION_EV_DROP_RETRYING:
        display_fx_static();   /* brief signal-loss snow, then retry */
        toast(now, "session dropped - reconnecting");
        nav_replace(SCR_CONNECTING, 0, now);
        return;
    case SSH_SESSION_EV_DROPPED:
        if (ssh_client_session_eof()) {
            /* Remote closed the channel cleanly (exit/logout): a
             * deliberate end, not a drop — release the transport now. */
            ssh_client_disconnect();
        }
        session_dropped(now);
        return;
    default:
        break;
    }

#ifdef CONFIG_DISPLAY_ISR_BENCH
    /* Render-ISR duty, 30 s cadence (rode the ssh read task before the
     * transport went terminal-free). */
    static uint64_t s_bench_last;
    if (!s_bench_last) s_bench_last = now;
    if (now - s_bench_last >= 30000) {
        uint32_t avg_cyc, max_cyc, chunks;
        display_render_bench_get(&avg_cyc, &max_cyc, &chunks);
        display_render_bench_reset();
        uint32_t elapsed_ms = (uint32_t)(now - s_bench_last);
        uint32_t chunks_per_sec = elapsed_ms ? (chunks * 1000u) / elapsed_ms : 0;
        /* duty = avg_cycles/chunk * chunks/s / core_hz; tenths of a percent. */
        uint32_t duty_pct_x10 = (uint32_t)(((uint64_t)avg_cyc * chunks_per_sec * 1000ULL)
                                            / 240000000ULL);
        ESP_LOGI("render_bench",
            "avg=%" PRIu32 " max=%" PRIu32 " duty=%" PRIu32 ".%" PRIu32 "%%",
            avg_cyc, max_cyc, duty_pct_x10 / 10, duty_pct_x10 % 10);
        s_bench_last = now;
    }
#endif

    render_session_chrome(now);
}

static void connecting_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                             uint64_t now)
{
    (void)ch;
    if (k == K_ESC) {
        /* Esc is Cancel while connecting and Back on the failed view. The
         * controller acks a cancel through EV_CANCELLED on the next tick. */
        if (s_conn.failed) {
            enter_home(now);
        } else {
            ssh_session_cancel(now);
            nav_invalidate();
        }
        return;
    }
    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind == UI_ACT_NONE) return;
    nav_invalidate();
    if (a.kind != UI_ACT_PRESS) return;
    const int idx = stored_host(s_conn.name);
    switch (a.id) {
    case ID_CANCEL:
        ssh_session_cancel(now);
        break;
    case ID_BACK:
        enter_home(now);
        break;
    case ID_AGAIN:
        if (idx >= 0) connect_checked(idx, now);
        break;
    case ID_EDIT:
        /* Home first: a finished edit pops back to the host list. */
        if (idx >= 0) {
            enter_home(now);
            enter_profile(now, idx);
        }
        break;
    default:
        break;
    }
}

/* Arrivals from other screens start on the connecting view. A reconnect
 * after a quiet session gets a fresh idle window. */
static void connecting_enter(intptr_t arg, uint64_t now)
{
    (void)arg;
    s_conn.failed      = false;
    s_conn.idle_cancel = false;
    session_guard_activity(now);
}

static void session_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                          uint64_t now)
{
    (void)ch;
    /* Every key goes to SSH except the menu triggers. */
    if (ev->type == CYBERDECK_INPUT_LONG_PRESS || k == K_F12) {
        menu_open(now);
        return;
    }
    /* Right-edge drag, content-follows-finger: down pulls older lines in. */
    if (ev->type == CYBERDECK_INPUT_SCROLL) {
        int rows = ui_drag_rows(&s_scroll_drag, ev->dy, font_height());
        if (rows) {
            vterm_scroll(rows);
            session_scroll_seen(now);
        }
        return;
    }

    if (ev->type != CYBERDECK_INPUT_KEY &&
        ev->type != CYBERDECK_INPUT_HIDKEY) return;

    /* Paging is local, but only in builds that have scrollback — otherwise
     * these keys belong to the remote. Capacity, not length: length is also
     * 0 on a fresh session. */
    if ((k == K_SCROLL_UP || k == K_SCROLL_DOWN) && vterm_scroll_capacity() > 0) {
        vterm_scroll_page(k == K_SCROLL_UP ? +1 : -1);
        session_scroll_seen(now);
        return;
    }

    /* Any other key snaps to live, and is still sent. */
    vterm_scroll_reset();

    if (ev->type == CYBERDECK_INPUT_HIDKEY) {
        /* The point of send — the one place a special key becomes wire
         * bytes. It encodes against the DECCKM state the remote set on
         * THIS vterm. */
        uint8_t seq[VTKEYS_MAX_LEN];
        size_t n = vtkeys_encode(vtkeys_from_hid(ev->key),
                                 vtkeys_mods_from_hid(ev->mods),
                                 vterm_app_cursor_keys(), seq, sizeof(seq));
        if (n) ssh_client_send(seq, n);
        return;
    }

    ssh_client_send(ev->buf, ev->len);
}

const nav_screen_t connecting_screen = {
    .name = "connecting", .enter = connecting_enter, .tick = connecting_tick,
    .input = connecting_input, .render = render_connecting,
    .chrome = NAV_CHROME_FULL,
    /* Callers arm the controller before navigating here. */
};

/* No render: the overlay belongs to the remote; the toast chip and the
 * scrollback indicator ride a self-managed pass in session_tick. */
const nav_screen_t session_screen = {
    .name = "session", .enter = session_enter, .resume = session_resume,
    .tick = session_tick, .input = session_input,
    .chrome = NAV_CHROME_NONE,
};
