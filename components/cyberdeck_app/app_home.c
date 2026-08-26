/*
 * app_home.c — HOME profile picker (ST_HOME) + the CRT power-off interlude
 * (ST_POWEROFF) that precedes it after a session ends.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "display_fx.h"
#include "keystore.h"
#include "wifi_manager.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

static struct {
    int      sel;                   /* selected HOME tile                */
    bool     kbd_bonded;            /* gates the "Pair keyboard" tile    */
    uint8_t  kon_idx;               /* Konami sequence progress          */
    uint64_t next_refresh;          /* next live-status re-render        */
    uint64_t poweroff_until;        /* POWEROFF: when the collapse ends  */
} s_home;

static const char *TAG = "app_home";

/* Trailing HOME tiles after the profiles: "New profile" only as a first-run
 * shortcut, "Pair keyboard" only while nothing is bonded, "Lock deck" (the
 * panic button belongs on HOME, not three taps deep) while a keystore
 * exists, Configuration always last. Order resolved by home_extras(). */
typedef enum { HX_NEW, HX_PAIR, HX_LOCK, HX_CONFIG } home_extra_t;
#define HOME_EXTRA_MAX 4

/* Keystore presence snapshot, refreshed on enter_home(): render_home runs
 * ~10 Hz and an ABSENT store would stat the filesystem every frame. Store
 * create/remove always route through other screens, so this stays true. */
static bool s_ks_present;

/* Resolve the trailing HOME tiles for the current state, in display order.
 * Returns the count; @p out must hold at least HOME_EXTRA_MAX entries. */
static int home_extras(home_extra_t *out)
{
    int n = 0;
    if (app.stored_count == 0)          out[n++] = HX_NEW;   /* first-run help */
    if (app.cfg.ble && !s_home.kbd_bonded)   out[n++] = HX_PAIR;  /* not yet bonded */
    if (s_ks_present)                   out[n++] = HX_LOCK;  /* panic button   */
    out[n++] = HX_CONFIG;                                    /* always, last   */
    return n;
}

/* Little ● / ○ LED then a label + value; returns the column just past the
 * value text so callers can append glyphs without layout knowledge. */
static int draw_status_led(int row, bool on, const char *label, const char *value)
{
    /* Live-link heartbeat: the dot contracts to ∙ (U+2219 — U+2022 is
     * byte-identical to ● in this font!) twice per ~1.6 s cycle. */
    uint32_t ph = app.anim_frame & 15;
    uint16_t cp = !on ? UI_LED_OFF
                : (ph == 0 || ph == 2) ? 0x2219 : UI_LED_ON;
    ui_pen(on ? OVERLAY_COL_GREEN : OVERLAY_COL_RED);
    ui_putch(2, row, cp, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(4, row, label, OVERLAY_ATTR_BOLD);   /* bold key, regular value */
    ui_puts(9, row, value, 0);
    return 9 + (int)strlen(value);
}

static void render_home(uint64_t now)
{
    (void)now;
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    /* ── Status HUD on the LEFT (rows 0-2) ─────────────────────────── */
    /* RSSI changes over seconds; don't hit the WiFi driver lock at the
     * 10 fps render cadence — refresh the cached value about once a second. */
    static bool     rssi_fresh = false;
    static uint32_t rssi_frame = 0;
    static int      rssi       = 0;
    if (!rssi_fresh || app.anim_frame - rssi_frame >= 10) {
        rssi_fresh = true;
        rssi_frame = app.anim_frame;
        rssi       = wifi_manager_get_rssi();
    }

    /* SSID clamped to a fixed field so the dBm suffix always fits and the
     * line stays clear of the right-aligned wordmark (starts at cols-10). */
    int sw = ui_cols() >= 97 ? 16 : 10;
    char net[48];
    snprintf(net, sizeof(net), "%-*.*s %s", sw, sw,
             wifi_manager_get_ssid()[0] ? wifi_manager_get_ssid() : "-",
             wifi_status_str());
    if (rssi < 0) {
        size_t nl = strlen(net);
        snprintf(net + nl, sizeof(net) - nl, "  %ddBm", rssi);
    }
    int netend = draw_status_led(0, wifi_manager_is_connected(), "NET", net);
    if (rssi < 0) {
        /* 4-step signal bar after the text; pen color tracks link quality. */
        uint16_t bar = rssi > -55 ? UI_BLOCK : rssi > -67 ? 0x2586
                     : rssi > -78 ? 0x2584  : 0x2582;
        ui_pen(rssi > -67 ? OVERLAY_COL_GREEN
             : rssi > -78 ? OVERLAY_COL_AMBER : OVERLAY_COL_RED);
        ui_putch(netend + 1, 0, bar, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    bool kbd = app.cfg.ble && app.cfg.ble->get_state && app.cfg.ble->get_state() == 4;
    const char *kn = (kbd && app.cfg.ble->get_name) ? app.cfg.ble->get_name() : "";
    char kbdinfo[64];
    snprintf(kbdinfo, sizeof(kbdinfo), "%-11s %s", ble_status_str(), kn);
    int ke = draw_status_led(1, kbd, "KBD", kbdinfo);

    /* Phone-presence chip after the KBD status. COLOR alone carries the
     * state (a fill-vs-hollow LED on top read as noise): green = near
     * (RSSI over the ~1-2 m gate), blue = in range but far, red =
     * enrolled but gone, amber = enroll mode (advertising, waiting for
     * the phone to pair). */
    if (app.cfg.presence &&
        (app.cfg.presence->enrolled() || app.cfg.presence->enroll_state() == 1)) {
        const bool adv  = app.cfg.presence->enroll_state() == 1;
        const bool ph   = !adv && app.cfg.presence->present();
        const bool nearby = ph && app.cfg.presence->is_near();
        ui_pen(adv    ? OVERLAY_COL_AMBER        /* enrolling            */
             : nearby ? OVERLAY_COL_GREEN        /* within arm's reach   */
             : ph     ? OVERLAY_COL_BLUE         /* in range, but far    */
                      : OVERLAY_COL_RED);        /* gone                 */
        ui_putch(ke + 2, 1, UI_LED_ON, 0);
        ui_puts(ke + 4, 1, "PHN", OVERLAY_ATTR_BOLD);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* All systems go: a small amber ☺ in the margin when net + keyboard up. */
    if (wifi_manager_is_connected() && kbd) {
        ui_pen(OVERLAY_COL_AMBER);
        ui_putch(0, 1, 0x263A, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    char ram[48];
    ram_stats(ram, sizeof(ram));
    ui_pen(OVERLAY_COL_BLUE);
    ui_putch(2, 2, UI_DIAMOND, 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    ui_puts(4, 2, "RAM", OVERLAY_ATTR_BOLD);
    ui_puts(9, 2, ram, 0);

    /* ── Title + version on the RIGHT (rows 0-1) ────────────────────── */
    ui_pen(OVERLAY_COL_CYAN);
    ui_puts(ui_cols() - (int)strlen("CYBERDECK") - 1, 0, "CYBERDECK",
            OVERLAY_ATTR_BOLD);
    char ver[24];
    snprintf(ver, sizeof(ver), "// %s", app.cfg.version ? app.cfg.version : "?");
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - (int)strlen(ver) - 1, 1, ver, 0);

    /* Wall clock under the version once SNTP delivers real time. */
    char clk[8];
    if (clock_str(clk, sizeof(clk)))
        ui_puts(ui_cols() - (int)strlen(clk) - 1, 2, clk, 0);
    ui_pen(OVERLAY_COL_DEFAULT);

    /* Tiles: one per profile, then the trailing extras (see home_extras). */
    home_extra_t xt[HOME_EXTRA_MAX];
    int nx = home_extras(xt);
    tilegrid_t g = picker_grid(app.profile_count + nx);
    app.grid = g;
    if (s_home.sel >= g.count) s_home.sel = g.count ? g.count - 1 : 0;
    if (app.profile_count + nx > g.ncols * g.nrows)
        ESP_LOGW(TAG, "%d profiles exceed one page; showing first %d",
                 app.profile_count, g.count - nx);

    for (int i = 0; i < g.count; i++) {
        int cx = tile_x(&g, i), cy = tile_y(&g, i);
        bool sel = (i == s_home.sel);
        if (i < app.profile_count) {
            const conn_profile_t *p = &app.profiles[i];
            char body[48];
            snprintf(body, sizeof(body), "%s@%s:%u%s",
                     p->user, p->host, (unsigned)p->port,
                     p->auth == STORAGE_AUTH_KEY ? "  [key]" : "");
            ui_pen(prof_accent(p->name));   /* stable per-name identity */
            ui_tile(cx, cy, g.tw, g.th, p->name, body, sel);
        } else {
            switch (xt[i - app.profile_count]) {
            case HX_NEW:
                ui_pen(OVERLAY_COL_GREEN);
                ui_tile(cx, cy, g.tw, g.th, "+ New profile", "add SSH host", sel);
                break;
            case HX_PAIR:
                ui_pen(OVERLAY_COL_CYAN);
                ui_tile(cx, cy, g.tw, g.th, "+ Pair keyboard",
                        "tap or long-press", sel);
                break;
            case HX_LOCK:
                ui_pen(OVERLAY_COL_AMBER);
                ui_tile(cx, cy, g.tw, g.th, "Lock deck", "PIN to wake", sel);
                break;
            case HX_CONFIG:
                ui_pen(OVERLAY_COL_CYAN);
                ui_tile(cx, cy, g.tw, g.th, "Configuration",
                        "wifi / profiles / more", sel);
                break;
            }
        }
    }

    /* Vacant tile sockets get a whisper of CRT static: a few dim braille
     * specks per empty slot, re-hashed every ~0.8 s. */
    ui_pen(OVERLAY_COL_BLUE);
    for (int i = g.count; i < g.ncols * g.nrows; i++) {
        for (int k = 0; k < 5; k++) {
            uint32_t h = (uint32_t)i * 97u + (uint32_t)k * 61u
                       + (app.anim_frame >> 3) * 31u;
            ui_putch(tile_x(&g, i) + (int)(h % (uint32_t)g.tw),
                     tile_y(&g, i) + (int)((h / 7u) % (uint32_t)g.th),
                     braille_noise(h), 0);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    if (app.profile_count == 0) {
        /* Below the two tiles that still render (pair + config). */
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts(3, g.y0 + g.th + 1,
                "no profiles - edit profiles.ini in storage", 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    /* Pac-Man marquee just above the footer — the sprite-glyph showcase. */
    draw_pacman(ui_rows() - 2);

    /* Footer legend; pairing hints only when the build has BLE, the lock
     * hotkey only with a keystore. An active toast owns the right edge —
     * draw_footer_lim clips clear of it. */
    char hint[96];
    snprintf(hint, sizeof(hint), "tap\xB7tap connect \xB7 %sR reload \xB7 W wifi%s",
             app.cfg.ble ? "hold pair \xB7 B pair \xB7 " : "",
             s_ks_present ? " \xB7 L lock" : "");
    if (app.toast[0]) {
        int tx = ui_cols() - ((int)strlen(app.toast) + 2) - 1;
        draw_footer_lim(hint, tx - 1);           /* -1: the taper cell */
        ui_pen(OVERLAY_COL_AMBER);
        ui_chip(tx - 1, ui_rows() - 1, UI_PL_L, app.toast, 0, 0);
        ui_pen(OVERLAY_COL_DEFAULT);
    } else {
        draw_footer(hint);
    }
}

static void home_enter(intptr_t arg, uint64_t now)
{
    (void)arg;
    s_home.kbd_bonded = ble_has_bond();   /* gate the "Pair keyboard" HOME tile */
    s_ks_present = keystore_state() != KEYSTORE_ABSENT;  /* "Lock deck" tile */
    pacman_reset();   /* session entry blanked the sprite slots */
    s_home.next_refresh = 0;
    /* Arriving on HOME counts as activity: a session drop or provisioning
     * toast must live its full lifetime before the rain paints over it. */
    saver_reset(now);
}

void enter_home(uint64_t now)
{
    nav_reset(SCR_HOME, 0, now);
}

/* If HOME tile @p slot is a trailing extra, return its home_extra_t, else -1. */
static int home_extra_kind(int slot)
{
    if (slot < app.profile_count) return -1;
    home_extra_t xt[HOME_EXTRA_MAX];
    int nx = home_extras(xt);
    int xi = slot - app.profile_count;
    return (xi >= 0 && xi < nx) ? (int)xt[xi] : -1;
}

/* Act on a trailing HOME extra tile; returns true if @p slot was one. */
static bool home_activate_extra(int slot, uint64_t now)
{
    switch (home_extra_kind(slot)) {
    case HX_NEW:    enter_profile(now, -1);              return true;
    case HX_PAIR:   if (app.cfg.ble) enter_pairing(now); return true;
    case HX_LOCK:                       /* panic: wipe MK, raise the gate */
        keystore_lock();
        app_creds_wipe();               /* ...and the hydrated copies */
        unlock_open_gate(now);
        return true;
    case HX_CONFIG: menu_open_config(now);               return true;
    default:        return false;
    }
}

/* Session teardown: hide the overlay so the CRT collapse plays over the last
 * live terminal frame, and enter HOME only once it finishes. Toasts set by
 * the caller survive — they are state, rendered when HOME appears. */
static void poweroff_enter(intptr_t arg, uint64_t now)
{
    ui_hide();
    ui_no_cursor();
    display_fx_collapse();
    s_home.poweroff_until = now + (uint64_t)arg * 17 + 80;
}

void enter_home_after_collapse(uint64_t now)
{
    display_fx_cfg_t c;
    display_fx_get(&c);
    if (!c.collapse) {
        enter_home(now);
        return;
    }
    nav_reset(SCR_POWEROFF, c.collapse_frames, now);
}

static void home_tick(uint64_t now)
{
    /* Expire toasts regardless of the saver, so a wake never flashes
     * a long-dead message. */
    if (app.toast[0] && now >= app.toast_until) app.toast[0] = '\0';

    /* Walk-away auto-lock (prototype): the phone was HERE, then unseen for
     * a minute, while the store sits unlocked — raise the gate, same path
     * as the L panic key. Armed on a sighting so a deck booted with the
     * phone already absent never self-locks out of nowhere; runs on HOME
     * only (v1 semantics: a live session holds the deck open). */
    if (app.cfg.presence && s_ks_present &&
        keystore_state() == KEYSTORE_UNLOCKED) {
        static bool armed;
        const cyberdeck_presence_ops_t *pr = app.cfg.presence;
        if (pr->present()) {
            armed = true;
        } else if (armed && pr->enrolled() && pr->age_ms() > 60000) {
            armed = false;
            keystore_lock();
            app_creds_wipe();
            unlock_open_gate(now);
            return;
        }
    }

    if (saver_tick_home(now)) return;
    if (now >= s_home.next_refresh) {
        s_home.next_refresh = now + ANIM_PERIOD_MS;   /* animation cadence */
        nav_invalidate();   /* live wifi/ble status */
    }
}

static void poweroff_tick(uint64_t now)
{
    /* Collapse finished (or was cut short by input) — bring HOME up. */
    if (now >= s_home.poweroff_until) enter_home(now);
}

static void home_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                       uint64_t now)
{
    /* Long-press anywhere = open pairing (works without a keyboard). */
    if (ev->type == CYBERDECK_INPUT_LONG_PRESS) {
        if (app.cfg.ble) enter_pairing(now);
        return;
    }
    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot < 0) return;                    /* gutter/margin: ignore */
        if (home_activate_extra(slot, now)) {    /* New / Pair / Config */
            /* handled */
        } else if (s_home.sel != slot) {              /* first tap: select + show */
            s_home.sel = slot;
            nav_invalidate();
        } else if (!wifi_manager_is_connected()) {
            toast(now, "wifi not connected yet");
            nav_invalidate();
        } else {                                 /* second tap on same tile */
            start_connect(slot, now, now);
        }
        return;
    }
    switch (k) {
    case K_UP: case K_DOWN: case K_LEFT: case K_RIGHT: {
        /* Konami progress rides along invisibly; EVERY arrow still
         * navigates. On mismatch, honor the overlapping UP-UP prefix
         * (an extra leading UP must not break the code). */
        static const ui_key_t KONAMI[8] = {
            K_UP, K_UP, K_DOWN, K_DOWN, K_LEFT, K_RIGHT, K_LEFT, K_RIGHT,
        };
        if (k == KONAMI[s_home.kon_idx])   s_home.kon_idx++;
        else if (k == K_UP)           s_home.kon_idx = (s_home.kon_idx == 2) ? 2 : 1;
        else                          s_home.kon_idx = 0;
        if (s_home.kon_idx == 8) {
            s_home.kon_idx = 0;
            display_bell();
            toast(now, "CHEAT ACCEPTED - RAM +30K (not really)");
        }
        int ns = tile_nav(&app.grid, s_home.sel, k);
        if (ns != s_home.sel) s_home.sel = ns;
        nav_invalidate();
        break;
    }
    case K_ENTER:
        if (home_activate_extra(s_home.sel, now)) {               /* New/Pair/Config */
            /* handled */
        } else if (app.profile_count > 0) {
            if (!wifi_manager_is_connected()) {
                toast(now, "wifi not connected yet");
                nav_invalidate();
            } else {
                start_connect(s_home.sel, now, now);
            }
        }
        break;
    case K_CHAR:
        if (ch == 'b' || ch == 'B') enter_pairing(now);
        else if (ch == 'n' || ch == 'N') {
            if (app.stored_count >= MAX_PROFILES - 1) {
                toast(now, "profile list full");
                nav_invalidate();
            } else {
                enter_profile(now, -1);
            }
        }
        else if (ch == 'r' || ch == 'R') { load_profiles(); nav_invalidate(); }
        else if (ch == 'w' || ch == 'W') { kick_wifi(); nav_invalidate(); }
        else if ((ch == 'l' || ch == 'L') && s_ks_present) {
            keystore_lock();                /* keyboard panic button */
            app_creds_wipe();
            unlock_open_gate(now);
        }
        else if ((ch == 'p' || ch == 'P') && app.cfg.presence) {
            /* Phone presence (prototype): P starts/cancels enroll mode when
             * no phone is stored; with one enrolled, P shows status and a
             * SECOND P within 2 s forgets (identity + bond) and drops
             * straight back into enroll — the clean re-pair gesture. The
             * phone side still needs its own "Forget This Device". */
            static uint64_t s_p_last;
            const cyberdeck_presence_ops_t *pr = app.cfg.presence;
            if (pr->enroll_state() == 1) {
                pr->enroll_stop();
                toast(now, "phone enroll cancelled");
            } else if (!pr->enrolled()) {
                pr->enroll_start();
                toast(now, "pair 'CYBERDECK' from the phone now");
            } else if (now - s_p_last < 2000) {
                pr->forget();
                pr->enroll_start();
                toast(now, "phone forgotten - pair again now");
            } else if (pr->present()) {
                toast(now, "phone here (%d dBm) - P again to re-pair",
                      pr->rssi());
            } else {
                const uint32_t age = pr->age_ms();
                if (age == UINT32_MAX)
                    toast(now, "phone not seen yet - P again to re-pair");
                else
                    toast(now, "phone away (%us) - P again to re-pair",
                          (unsigned)(age / 1000));
            }
            s_p_last = now;
            nav_invalidate();
        }
        break;
    default:
        break;
    }
}

static void poweroff_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                           uint64_t now)
{
    (void)ev; (void)k; (void)ch;
    /* Any input skips the rest of the power-off animation. */
    enter_home(now);
}

const nav_screen_t home_screen = {
    .name = "home", .enter = home_enter, .tick = home_tick,
    .input = home_input, .render = render_home, .chrome = NAV_CHROME_NONE,
};

/* No render: the collapse fx plays over the dead terminal frame. */
const nav_screen_t poweroff_screen = {
    .name = "poweroff", .enter = poweroff_enter, .tick = poweroff_tick,
    .input = poweroff_input, .chrome = NAV_CHROME_NONE,
};
