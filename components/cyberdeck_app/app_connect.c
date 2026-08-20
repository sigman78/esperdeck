/*
 * app_connect.c — SSH connect lifecycle: CONNECTING (armed / async worker /
 * retry countdown) and the live SESSION (toast chip, drop handling).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "display_fx.h"
#include "font.h"        /* font_height() — drag pixels to terminal rows */
#include "keystore.h"
#include "ssh_client.h"
#include "vterm.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"   /* key-PEM buffers (idfsim stubs it) */

static void render_connecting(const char *msg, uint64_t now)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_screen_header("CONNECTING", "// SSH DECK");

    const conn_profile_t *p = &app.conn.active;
    int cy = ui_rows() / 2;

    char line[96];
    size_t ll = (size_t)snprintf(line, sizeof(line), "%s  %s@%s:%u",
                                 msg, p->user, p->host, (unsigned)p->port);
    if (app.conn.attempt > 0 && ll < sizeof(line))
        snprintf(line + ll, sizeof(line) - ll, "  (attempt %d)", app.conn.attempt);
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
    if (app.conn.armed && app.conn.connect_at > now && app.cfg.ssh_retry_delay_ms) {
        /* Retry wait: an amber bar drains toward the reconnect moment, so
         * the delay reads as a countdown instead of a stalled connect. */
        uint64_t remain = app.conn.connect_at - now;
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
        ui_pen(app.conn.cancelled ? OVERLAY_COL_AMBER : prof_accent(p->name));
        for (int i = 0; i < bw; i++)
            ui_putch(bx + i, cy + 1, grad[(i + app.anim_frame) % 7], 0);
        if (app.conn.connecting) {
            /* Elapsed seconds right of the bar: a stalled handshake at 40 s
             * should look different from one that just started. */
            ui_pen(OVERLAY_COL_BLUE);
            ui_printf(bx + bw + 2, cy + 1, 0, "%2us",
                      (unsigned)((now - app.conn.started) / 1000));
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_footer("tap or Esc to cancel");
    ui_no_cursor();
    ui_present();
}

#define SCROLLBAR_LINGER_MS  1400

static uint64_t s_scrollbar_until;   /* 0 = not showing */

/* Drag travel not yet spent, in pixel-percent (px * SCROLL_SPEED_PCT). */
static int s_scroll_accum;

#ifndef CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT
#define CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT 100
#endif
#define SCROLL_SPEED_PCT  CONFIG_INPUT_TOUCH_SCROLL_SPEED_PCT

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
        if (app.state == ST_SESSION) ui_hide();
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
                 OVERLAY_ATTR_INVERSE);
    } else {
        ui_chip(x - 1, 0, UI_PL_L, app.toast, 0, 0);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_present();
}

/* Fetch the pinned fingerprint (if any) for the active profile's host. */
static void load_pinned_fp(void)
{
    if (storage_known_host_get(app.conn.active.host, app.conn.active.port,
                               app.conn.pinned_fp, sizeof(app.conn.pinned_fp)) != ESP_OK)
        app.conn.pinned_fp[0] = '\0';
}

/* Arm a connect: one frame of "Connecting", then do it. */
static void arm_connect(uint64_t not_before, uint64_t now)
{
    app.conn.connect_at = not_before;
    app.conn.armed      = true;
    app.state    = ST_CONNECTING;
    render_connecting(now < not_before ? "Retrying" : "Connecting to", now);
}

/* Arm a connect to profile @p idx, snapshotting it into app.conn.active. */
void start_connect(int idx, uint64_t not_before, uint64_t now)
{
    app.conn.attempt = 0;
    app.conn.active  = app.profiles[idx];
    load_pinned_fp();
    /* Key profile + locked store: unlock first — the PIN pad re-arms this
     * connect on success (the lazy on-first-key-use trigger). Gates even
     * when THIS key is still a bare .pem: a present store means keys are
     * protected or pending adoption (the unlock performs it), so plaintext
     * must not silently bypass the lock. ABSENT store = feature off. */
    if (app.conn.active.auth == STORAGE_AUTH_KEY &&
        keystore_state() == KEYSTORE_LOCKED) {
        unlock_open(now, true);
        return;
    }
    arm_connect(not_before, now);
}

/* Re-arm a connect to the ACTIVE snapshot (unlock-screen resume). */
void connect_resume_active(uint64_t now)
{
    app.conn.attempt = 0;
    arm_connect(now, now);
}

/* Re-arm a connect to the active snapshot with @p fp pre-pinned — the
 * hostkey prompt's trust path (explicit user action, not a retry). */
void connect_arm_pinned(const char *fp, uint64_t now)
{
    snprintf(app.conn.pinned_fp, sizeof(app.conn.pinned_fp), "%s", fp);
    app.conn.attempt = 0;
    arm_connect(now, now);
}

/* Re-arm an automatic reconnect to the ACTIVE snapshot (advances the
 * visible attempt counter; survives edits/deletes of the stored list). */
static void start_reconnect(uint64_t not_before, uint64_t now)
{
    app.conn.attempt++;
    load_pinned_fp();
    arm_connect(not_before, now);
}

/* Session died and we are NOT auto-reconnecting: the classic modem death
 * rattle, with link time and the drop reason ("" on a clean EOF). */
void session_dropped(uint64_t now)
{
    /* A drop can yank the user out of a mid-drag reorder; discard the
     * uncommitted permutation or a later delete would persist it. */
    menu_abort_reorder();
    uint32_t dur = (uint32_t)((now - app.conn.session_start) / 1000);
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

/* Hand the screen back to a live session. Every full-screen modal parks the
 * terminal cursor (ui_no_cursor) and a vterm flush only happens when the
 * host sends bytes — without the explicit refresh the cursor stays gone
 * until the first input after leaving the menu/pairing screen. */
void session_resume(void)
{
    app.state = ST_SESSION;
    ui_hide();
    vterm_cursor_refresh();
}

static void enter_session(uint64_t now)
{
    app.conn.session_start = now;
    app.conn.attempt       = 0;   /* a future drop counts retries from 1 again */
    display_fx_wipe();     /* raster-reveal the fresh session */
    session_resume();
    /* The terminal was cleared inside ssh_client_connect() before the read
     * task spawned — doing it here would race that task inside vterm. */
    static const char *const HELLO[] = {
        "jacked in", "link up", "uplink established",
        "handshake clean", "you're in",
    };
    toast(now, "%s - F12 or long-press for menu",
          HELLO[app.anim_frame % (sizeof(HELLO) / sizeof(HELLO[0]))]);
    app.toast_ok = true;       /* garnish with the spinner-to-checkmark */
    render_session_chrome(now);
}

/* Key PEMs are read on the shell task (the connect worker has a PSRAM
 * stack and must not touch littlefs — flash I/O asserts there) and must
 * outlive the async connect: the worker reads the cfg strings during the
 * handshake. The PRIVATE key buffer is INTERNAL SRAM — never PSRAM, the
 * external bus is probeable (docs/storage_auth.md RAM hygiene) — and is
 * wiped the moment the connect worker finishes; the .pub is public. */
enum { KEY_PEM_MAX = 8192, PUB_PEM_MAX = 2048 };
static char *s_key_pem = NULL;
static char *s_pub_pem = NULL;

/* Connect-time credential (password or key passphrase). The profile
 * snapshot may predate the unlock (lazy gate) and so miss its diverted
 * secret — resolve from the bundle here, .bss (internal SRAM), wiped with
 * the key PEM once the handshake is over. */
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

/* Kick off the connect on a worker task (non-blocking) so the shell keeps
 * ticking — the "Connecting" bar animates and a tap/ESC can cancel. */
static void do_connect_start(uint64_t now)
{
    const conn_profile_t *p = &app.conn.active;

    ssh_config_t cfg = {
        .host        = p->host,
        .port        = p->port,
        .username    = p->user,
        .expected_fp = app.conn.pinned_fp[0] ? app.conn.pinned_fp : NULL,
    };
    if (p->auth == STORAGE_AUTH_KEY) {
        /* Locked store slipped past the start_connect gate (reconnect,
         * re-arm, or a lock trigger fired since): same bounce — covers
         * bare .pem keys the wrapped-only INVALID_STATE path below can't. */
        if (keystore_state() == KEYSTORE_LOCKED) {
            unlock_open(now, true);
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
            /* Locked store slipped past the start_connect gate (e.g. an
             * auto-reconnect after a future lock trigger): same bounce. */
            unlock_open(now, true);
            return;
        }
        if (ge != ESP_OK) {
            toast(now, "key '%s' unreadable", p->key_id);
            enter_home(now);
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

    if (ssh_client_connect_start(&cfg) != ESP_OK) {
        wipe_key_pem();
        toast(now, "connect busy - try again");
        enter_home(now);
        return;
    }
    app.conn.connecting  = true;
    app.conn.cancelled   = false;
    app.conn.started     = now;
    app.next_anim = 0;
    render_connecting("Connecting to", now);
}

/* Handle the async connect result once the worker finishes. */
static void do_connect_finish(uint64_t now, esp_err_t err)
{
    /* Handshake over either way — libssh2 has derived what it needs, so
     * the plaintext key must not linger for the session's lifetime. */
    wipe_key_pem();
    if (app.conn.cancelled) {
        if (err == ESP_OK) ssh_client_disconnect();   /* it connected as we cancelled */
        app.conn.cancelled = false;
        toast(now, "cancelled");
        enter_home(now);
        return;
    }
    switch (err) {
    case ESP_OK:
        enter_session(now);
        break;

    case SSH_ERR_HOSTKEY_UNKNOWN:
        hostkey_open(false);
        break;

    case SSH_ERR_HOSTKEY_MISMATCH:
        hostkey_open(true);
        break;

    case SSH_ERR_AUTH:
        toast_for(now, ERR_TOAST_MS, "auth failed: %.40s",
                  ssh_client_last_error());
        enter_home(now);
        break;

    default:
        if (app.cfg.auto_reconnect) {
            toast(now, "connect failed - retrying");
            start_reconnect(now + app.cfg.ssh_retry_delay_ms, now);
        } else {
            toast_for(now, ERR_TOAST_MS, "failed: %.44s",
                      ssh_client_last_error());
            enter_home(now);
        }
        break;
    }
}

void connecting_tick(uint64_t now)
{
    if (app.conn.armed && now >= app.conn.connect_at) {
        app.conn.armed = false;
        do_connect_start(now);           /* launches worker; returns at once */
    } else if (app.conn.armed) {                /* retry pre-delay: drain the bar */
        if (now >= app.next_anim) {
            app.next_anim = now + ANIM_PERIOD_MS;
            render_connecting("Retrying", now);
        }
    } else if (app.conn.connecting) {
        if (ssh_client_connect_ready()) {
            app.conn.connecting = false;
            do_connect_finish(now, ssh_client_connect_take_result());
        } else if (now >= app.next_anim) {  /* keep the bar alive while it runs */
            app.next_anim = now + ANIM_PERIOD_MS;
            render_connecting(app.conn.cancelled ? "Cancelling" : "Connecting to", now);
        }
    }
}

void session_tick(uint64_t now)
{
    if (!ssh_client_is_connected()) {
        if (ssh_client_session_eof()) {
            /* Remote closed the channel cleanly (exit/logout): a
             * deliberate end, not a drop — never auto-reconnect. */
            ssh_client_disconnect();
            session_dropped(now);
        } else if (app.cfg.auto_reconnect) {
            display_fx_static();   /* brief signal-loss snow, then retry */
            toast(now, "session dropped - reconnecting");
            start_reconnect(now + app.cfg.ssh_retry_delay_ms, now);
        } else {
            session_dropped(now);
        }
        return;
    }
    render_session_chrome(now);
}

void connecting_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    (void)ch;
    /* ESC (keyboard) or any tap/long-press (touch) cancels. */
    if (k == K_ESC || ev->type == CYBERDECK_INPUT_TAP ||
        ev->type == CYBERDECK_INPUT_LONG_PRESS) {
        if (app.conn.armed) {                          /* still in the pre-delay wait */
            app.conn.armed = false;
            toast(now, "cancelled");     /* same ack as the mid-connect path */
            enter_home(now);
        } else if (app.conn.connecting && !app.conn.cancelled) {
            app.conn.cancelled = true;
            ssh_client_connect_cancel();        /* best-effort unblock */
            render_connecting("Cancelling", now);
        }
    }
}

void session_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    (void)ch;
    /* Every byte goes to SSH except the menu triggers. */
    if (ev->type == CYBERDECK_INPUT_LONG_PRESS ||
        (ev->type == CYBERDECK_INPUT_KEY && k == K_F12)) {
        menu_open(now);
        return;
    }
    /* Right-edge drag, content-follows-finger: down pulls older lines in.
     * Pixels accumulate because the touch task polls at 50 ms — converting
     * each small dy on its own would floor to zero and a slow drag would
     * never scroll. */
    if (ev->type == CYBERDECK_INPUT_SCROLL) {
        s_scroll_accum += (int)ev->dy * SCROLL_SPEED_PCT;
        const int per_row = font_height() * 100;
        int rows = s_scroll_accum / per_row;
        if (rows) {
            s_scroll_accum -= rows * per_row;
            vterm_scroll(rows);
            session_scroll_seen(now);
        }
        return;
    }

    if (ev->type != CYBERDECK_INPUT_KEY) return;

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

    ssh_client_send(ev->buf, ev->len);
}
