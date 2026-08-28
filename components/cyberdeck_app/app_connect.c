/*
 * app_connect.c — the thin connect/session screens. CONNECTING renders
 * controller state (armed / worker / retry countdown). SESSION owns the
 * live chrome (toast chip, scrollback bar) and input. Connect policy —
 * retry, key resolution, hostkey stops — lives in ssh_session.c
 * (extensibility item 7).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "display.h"     /* display_get_text_size — the PTY geometry */
#include "display_fx.h"
#include "font.h"        /* font_height() — drag pixels to terminal rows */
#include "keystore.h"
#include "ssh_client.h"
#include "ssh_session.h"
#include "vterm.h"
#include "vtkeys.h"      /* the session encodes HIDKEY events at send */

#include <stdio.h>
#include <string.h>

#ifdef CONFIG_DISPLAY_ISR_BENCH
#include "esp_log.h"
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

static void render_connecting(uint64_t now)
{
    /* The status word falls out of the controller state. */
    ssh_session_state_t st = ssh_session_state();
    const char *msg = st == SSH_SESSION_CANCELLING ? "Cancelling"
        : (st == SSH_SESSION_ARMED && now < ssh_session_retry_at())
            ? "Retrying" : "Connecting to";

    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_screen_header("CONNECTING", "// SSH DECK");

    const conn_profile_t *p = ssh_session_profile();
    int cy = ui_rows() / 2;
    int attempt = ssh_session_attempt();

    char line[96];
    size_t ll = (size_t)snprintf(line, sizeof(line), "%s  %s@%s:%u",
                                 msg, p->user, p->host, (unsigned)p->port);
    if (attempt > 0 && ll < sizeof(line))
        snprintf(line + ll, sizeof(line) - ll, "  (attempt %d)", attempt);
    /* A long host/user can outgrow a narrow grid: truncate and pin left so
     * the leading status word always survives. */
    int maxw = ui_cols() - 8;
    if ((int)strlen(line) > maxw) line[maxw] = '\0';
    int lx = (ui_cols() - (int)strlen(line)) / 2;
    if (lx < 4) lx = 4;
    ui_pen(prof_accent(p->name));   /* carries the tile's identity color */
    ui_putch(lx - 2, cy - 1, UI_DIAMOND, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(lx, cy - 1, line, 0);

    int bw = 42, bx = (ui_cols() - bw) / 2;
    uint64_t retry_at = ssh_session_retry_at();
    if (st == SSH_SESSION_ARMED && retry_at > now &&
        app.cfg.ssh_retry_delay_ms) {
        /* During the retry wait, an amber bar drains toward the reconnect
         * moment. The delay reads as a countdown, not a stalled connect. */
        uint64_t remain = retry_at - now;
        if (remain > app.cfg.ssh_retry_delay_ms)
            remain = app.cfg.ssh_retry_delay_ms;
        int filled = (int)(remain * (uint64_t)bw / app.cfg.ssh_retry_delay_ms);
        ui_pen(OVERLAY_COL_AMBER);
        for (int i = 0; i < bw; i++)
            ui_putch(bx + i, cy + 1, i < filled ? UI_BLOCK : UI_SHADE1, 0);
    } else {
        /* Activity bar: ░▒▓█▓▒░ gradient scrolling with the animation frame. */
        static const uint16_t grad[7] = {
            UI_SHADE1, UI_SHADE2, UI_SHADE3, UI_BLOCK,
            UI_SHADE3, UI_SHADE2, UI_SHADE1
        };
        ui_pen(st == SSH_SESSION_CANCELLING ? OVERLAY_COL_AMBER
                                            : prof_accent(p->name));
        for (int i = 0; i < bw; i++)
            ui_putch(bx + i, cy + 1, grad[(i + app.anim_frame) % 7], 0);
        if (st == SSH_SESSION_CONNECTING) {
            /* Elapsed seconds right of the bar: a stalled handshake at 40 s
             * should look different from one that just started. */
            ui_pen(OVERLAY_COL_BLUE);
            ui_printf(bx + bw + 2, cy + 1, 0, "%2us",
                      (unsigned)((now - ssh_session_attempt_started()) / 1000));
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);
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

    /* Amber chip with a powerline taper — same ui_chip as the HOME toast,
     * so the one element shown on both screens renders identically. */
    int x = ui_cols() - ((int)strlen(app.toast) + 2) - 1;
    ui_pen(OVERLAY_COL_AMBER);
    if (app.toast_ok) {
        /* Success garnish: braille spinner for ~0.8 s, then a checkmark —
         * the connect toast "completes" in front of you. */
        uint32_t el = (uint32_t)(TOAST_MS - (app.toast_until - now));
        char pad[68];
        snprintf(pad, sizeof(pad), "  %s", app.toast);
        ui_chip(x - 3, 0, UI_PL_L, pad, 0, 0);
        ui_putch(x - 1, 0, el < 800 ? spinner_glyph(app.anim_frame) : 0x2713,
                 UI_BAR);
    } else {
        ui_chip(x - 1, 0, UI_PL_L, app.toast, 0, 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_present();
}

/* Arm a connect screen-side: the controller is already armed. */
static void show_connecting(uint64_t now)
{
    if (nav_current() == SCR_CONNECTING) nav_invalidate();
    else nav_replace(SCR_CONNECTING, 0, now);
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
    /* A drop can yank the user out of a mid-drag reorder; discard the
     * uncommitted permutation or a later delete would persist it. */
    menu_abort_reorder();
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
    app.toast_ok = true;       /* garnish with the spinner-to-checkmark */
    render_session_chrome(now);
}

/* Route a controller event to navigation and toasts. True when the
 * event navigated away (the caller stops its tick). */
static bool handle_session_event(ssh_session_event_t ev, uint64_t now)
{
    switch (ev) {
    case SSH_SESSION_EV_CONNECTED:
        nav_replace(SCR_SESSION, 0, now);
        return true;
    case SSH_SESSION_EV_NEED_UNLOCK:
        unlock_open(now, true);
        return true;
    case SSH_SESSION_EV_KEY_UNREADABLE:
        toast(now, "key '%s' unreadable", ssh_session_profile()->key_id);
        enter_home(now);
        return true;
    case SSH_SESSION_EV_HOSTKEY_UNKNOWN:
        hostkey_open(false, now);
        return true;
    case SSH_SESSION_EV_HOSTKEY_MISMATCH:
        hostkey_open(true, now);
        return true;
    case SSH_SESSION_EV_AUTH_FAILED:
        toast_for(now, ERR_TOAST_MS, "auth failed: %.40s",
                  ssh_client_last_error());
        enter_home(now);
        return true;
    case SSH_SESSION_EV_BUSY:
        toast(now, "connect busy - try again");
        enter_home(now);
        return true;
    case SSH_SESSION_EV_RETRYING:
        toast(now, "connect failed - retrying");
        return false;                    /* countdown renders here */
    case SSH_SESSION_EV_FAILED:
        toast_for(now, ERR_TOAST_MS, "failed: %.44s",
                  ssh_client_last_error());
        enter_home(now);
        return true;
    case SSH_SESSION_EV_CANCELLED:
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
    if (now >= app.next_anim) {          /* keep the bar/countdown alive */
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
    /* ESC (keyboard) or any tap/long-press (touch) cancels. The
     * controller acks through EV_CANCELLED on the next tick. */
    if (k == K_ESC || ev->type == CYBERDECK_INPUT_TAP ||
        ev->type == CYBERDECK_INPUT_LONG_PRESS) {
        ssh_session_cancel(now);
        nav_invalidate();
    }
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
    .name = "connecting", .tick = connecting_tick,
    .input = connecting_input, .render = render_connecting,
    .chrome = NAV_CHROME_FULL,
    /* No enter hook: callers arm the controller before navigating here. */
};

/* No render: the overlay belongs to the remote; the toast chip and the
 * scrollback indicator ride a self-managed pass in session_tick. */
const nav_screen_t session_screen = {
    .name = "session", .enter = session_enter, .resume = session_resume,
    .tick = session_tick, .input = session_input,
    .chrome = NAV_CHROME_NONE,
};
