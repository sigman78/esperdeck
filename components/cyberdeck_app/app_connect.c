/*
 * app_connect.c — SSH connect lifecycle: CONNECTING (armed / async worker /
 * retry countdown) and the live SESSION (toast chip, drop handling).
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "app_menu_defs.h"   /* MS_REORDER: a drop discards a mid-drag reorder */
#include "display_fx.h"
#include "ssh_client.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"   /* key-PEM buffers (idfsim stubs it) */

void render_connecting(const char *msg, uint64_t now)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    draw_screen_header("CONNECTING", "// SSH DECK");

    const conn_profile_t *p = &app.active;
    int cy = ui_rows() / 2;

    char line[96];
    size_t ll = (size_t)snprintf(line, sizeof(line), "%s  %s@%s:%u",
                                 msg, p->user, p->host, (unsigned)p->port);
    if (app.connect_attempt > 0 && ll < sizeof(line))
        snprintf(line + ll, sizeof(line) - ll, "  (attempt %d)",
                 app.connect_attempt);
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
    if (app.connect_armed && app.connect_at > now && app.cfg.ssh_retry_delay_ms) {
        /* Retry wait: an amber bar drains toward the reconnect moment, so
         * the delay reads as a countdown instead of a stalled connect. */
        uint64_t remain = app.connect_at - now;
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
        ui_pen(app.connect_cancelled ? OVERLAY_COL_AMBER : prof_accent(p->name));
        for (int i = 0; i < bw; i++)
            ui_putch(bx + i, cy + 1, grad[(i + app.anim_frame) % 7], 0);
        if (app.connecting) {
            /* Elapsed seconds right of the bar: a stalled handshake at 40 s
             * should look different from one that just started. */
            ui_pen(OVERLAY_COL_BLUE);
            ui_printf(bx + bw + 2, cy + 1, 0, "%2us",
                      (unsigned)((now - app.connect_started) / 1000));
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_footer("tap or Esc to cancel");
    ui_no_cursor();
    ui_present();
}

static void render_session_toast(uint64_t now)
{
    if (now >= app.toast_until || !app.toast[0]) {
        if (app.state == ST_SESSION) ui_hide();
        return;
    }
    /* Amber chip with a powerline taper — the old hard black-on-yellow was
     * the only element outside the shell palette. Same ui_chip as the HOME
     * toast, so the one element shown on both screens renders identically. */
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    int x = ui_cols() - ((int)strlen(app.toast) + 2) - 1;
    ui_pen(OVERLAY_COL_AMBER);
    if (app.toast_ok) {
        /* Success garnish: the braille spinner works for ~0.8 s, then snaps
         * to a checkmark — the connect toast "completes" in front of you.
         * ST_SESSION re-renders this every tick, so the animation is free.
         * Two pad cells lead the text; the first holds the glyph. */
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

/* Arm a connect to profile idx: one frame of "Connecting", then do it.
 * Snapshots the profile into app.active — every later connect/session path
 * reads the snapshot, so list mutations can't redirect a live session. */
void start_connect(int idx, uint64_t not_before, uint64_t now)
{
    app.connect_attempt = 0;
    app.connect_idx   = idx;
    app.active        = app.profiles[idx];
    app.connect_at    = not_before;
    app.connect_armed = true;
    app.state         = ST_CONNECTING;

    /* Pinned fingerprint, if we have one for this host. */
    if (storage_known_host_get(app.active.host, app.active.port,
                               app.pinned_fp, sizeof(app.pinned_fp)) != ESP_OK)
        app.pinned_fp[0] = '\0';

    render_connecting(now < not_before ? "Retrying" : "Connecting to", now);
}

/* Re-arm an automatic reconnect to the ACTIVE profile snapshot (advances the
 * visible attempt counter; survives edits/deletes of the stored list). */
static void start_reconnect(uint64_t not_before, uint64_t now)
{
    app.connect_attempt++;
    app.connect_at    = not_before;
    app.connect_armed = true;
    app.state         = ST_CONNECTING;

    if (storage_known_host_get(app.active.host, app.active.port,
                               app.pinned_fp, sizeof(app.pinned_fp)) != ESP_OK)
        app.pinned_fp[0] = '\0';

    render_connecting(now < not_before ? "Retrying" : "Connecting to", now);
}

/* Session died and we are NOT auto-reconnecting: the classic modem death
 * rattle, with link time and the drop reason ssh_client recorded ("" on a
 * clean EOF — a plain logout stays calm and short). */
void session_dropped(uint64_t now)
{
    /* A drop can yank the user out of ANY in-session menu screen — including
     * a mid-drag reorder. Discard the uncommitted in-RAM permutation now, or
     * a later delete_profile_at() would silently persist it. */
    if (app.menu_screen == MS_REORDER && app.reorder_grab >= 0) {
        app.reorder_grab = -1;
        load_profiles();
    }
    uint32_t dur = (uint32_t)((now - app.session_start) / 1000);
    const char *why = ssh_client_last_error();
    display_bell();
    if (why[0]) display_fx_static();   /* transport error: signal-loss snow */
    if (why[0])
        toast_for(now, ERR_TOAST_MS, "NO CARRIER (%02u:%02u) - %.36s",
                  dur / 60, dur % 60, why);
    else
        toast(now, "NO CARRIER (%02u:%02u)", dur / 60, dur % 60);
    enter_home_after_melt(now);        /* HOME melts down over the dead frame */
}

static void enter_session(uint64_t now)
{
    app.state = ST_SESSION;
    app.session_start   = now;
    app.connect_attempt = 0;   /* a future drop counts retries from 1 again */
    /* The CONNECTING overlay melts away, revealing the live session already
     * beneath it. Keep the overlay up until the melt finishes — hiding it
     * then is visually a no-op (session_tick does it); with the melt
     * disabled, hide right here. */
    display_fx_melt_away();
    if (display_fx_melt_active()) app.session_melting = true;
    else                          ui_hide();
    /* The terminal was cleared inside ssh_client_connect() before the read
     * task spawned — doing it here would race that task inside vterm. */
    static const char *const HELLO[] = {
        "jacked in", "link up", "uplink established",
        "handshake clean", "you're in",
    };
    toast(now, "%s - F12 or long-press for menu",
          HELLO[app.anim_frame % (sizeof(HELLO) / sizeof(HELLO[0]))]);
    app.toast_ok = true;       /* garnish with the spinner-to-checkmark */
    /* While melting, the overlay still holds the sliding CONNECTING frame —
     * painting the chip now would wipe it (ui_clear). The toast outlives
     * the melt; session_tick brings the chip up when the sheet is gone. */
    if (!app.session_melting) render_session_toast(now);
}

/* Kick off the connect on a worker task (non-blocking) so the shell keeps
 * ticking — the "Connecting" bar animates and a tap/ESC can cancel. */
static void do_connect_start(uint64_t now)
{
    /* Key PEMs are read HERE, on the shell task: the connect worker has a
     * PSRAM stack and must not touch littlefs (flash I/O asserts there).
     * Buffers are PSRAM data (fine to read from any task), allocated once
     * and reused; they must outlive the connect, so no free on finish. */
    enum { KEY_PEM_MAX = 8192, PUB_PEM_MAX = 2048 };
    static char *key_pem = NULL;
    static char *pub_pem = NULL;
    const conn_profile_t *p = &app.active;

    ssh_config_t cfg = {
        .host        = p->host,
        .port        = p->port,
        .username    = p->user,
        .expected_fp = app.pinned_fp[0] ? app.pinned_fp : NULL,
    };
    if (p->auth == STORAGE_AUTH_KEY) {
        if (!key_pem) key_pem = heap_caps_malloc(KEY_PEM_MAX, MALLOC_CAP_SPIRAM);
        if (!pub_pem) pub_pem = heap_caps_malloc(PUB_PEM_MAX, MALLOC_CAP_SPIRAM);
        size_t klen = 0;
        if (!key_pem || !pub_pem ||
            storage_get_key(p->key_id, key_pem, KEY_PEM_MAX, &klen) != ESP_OK) {
            toast(now, "key '%s' unreadable", p->key_id);
            enter_home(now);
            return;
        }
        cfg.private_key_pem = key_pem;
        cfg.passphrase      = p->password[0] ? p->password : NULL;

        char pub_path[160];   /* optional .pub beside the key */
        snprintf(pub_path, sizeof(pub_path), "%s/keys/%s.pub",
                 storage_platform_mount_point(), p->key_id);
        FILE *pf = fopen(pub_path, "r");
        if (pf) {
            size_t n = fread(pub_pem, 1, PUB_PEM_MAX - 1, pf);
            fclose(pf);
            if (n > 0) { pub_pem[n] = '\0'; cfg.public_key_pem = pub_pem; }
        }
    } else {
        cfg.password = p->password;
    }

    if (ssh_client_connect_start(&cfg) != ESP_OK) {
        toast(now, "connect busy - try again");
        enter_home(now);
        return;
    }
    app.connecting        = true;
    app.connect_cancelled = false;
    app.connect_started   = now;
    app.next_anim         = 0;
    render_connecting("Connecting to", now);
}

/* Handle the async connect result once the worker finishes. */
static void do_connect_finish(uint64_t now, esp_err_t err)
{
    if (app.connect_cancelled) {
        if (err == ESP_OK) ssh_client_disconnect();   /* it connected as we cancelled */
        app.connect_cancelled = false;
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
    if (app.connect_armed && now >= app.connect_at) {
        app.connect_armed = false;
        do_connect_start(now);           /* launches worker; returns at once */
    } else if (app.connect_armed) {       /* retry pre-delay: drain the bar */
        if (now >= app.next_anim) {
            app.next_anim = now + ANIM_PERIOD_MS;
            render_connecting("Retrying", now);
        }
    } else if (app.connecting) {
        if (ssh_client_connect_ready()) {
            app.connecting = false;
            do_connect_finish(now, ssh_client_connect_take_result());
        } else if (now >= app.next_anim) {  /* keep the bar alive while it runs */
            app.next_anim = now + ANIM_PERIOD_MS;
            render_connecting(app.connect_cancelled ? "Cancelling"
                                                    : "Connecting to", now);
        }
    }
}

void session_tick(uint64_t now)
{
    /* Retire the CONNECTING overlay once its melt-away has finished —
     * every column already shows the terminal, so the hide is invisible.
     * Until then the overlay is strictly hands-off: any repaint (like the
     * toast chip, which starts with ui_clear) would wipe the sliding
     * sheet mid-melt. */
    if (app.session_melting) {
        if (display_fx_melt_active()) {
            /* fall through: connection watchdog still runs during the melt */
        } else {
            app.session_melting = false;
            ui_hide();
        }
    }

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
    if (!app.session_melting) render_session_toast(now);
}

void connecting_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    (void)ch;
    /* ESC (keyboard) or any tap/long-press (touch) cancels. */
    if (k == K_ESC || ev->type == CYBERDECK_INPUT_TAP ||
        ev->type == CYBERDECK_INPUT_LONG_PRESS) {
        if (app.connect_armed) {                /* still in the pre-delay wait */
            app.connect_armed = false;
            toast(now, "cancelled");     /* same ack as the mid-connect path */
            enter_home(now);
        } else if (app.connecting && !app.connect_cancelled) {
            app.connect_cancelled = true;
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
    if (ev->type == CYBERDECK_INPUT_KEY)
        ssh_client_send(ev->buf, ev->len);
}
