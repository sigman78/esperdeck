/*
 * app_home.c — HOME host picker (SCR_HOME) + the CRT power-off interlude
 * (SCR_POWEROFF) that precedes it after a session ends. HOME draws with
 * the kit's parts: a tap connects, a hold opens the host page (app_host.c).
 */

#include "app_internal.h"
#include "session_guard.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "cyberdeck_plugin.h"
#include "display_fx.h"
#include "keystore.h"
#include "wifi_manager.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

/* Region ids. Hosts and plugin tiles count up from ID_TILE by tile index. */
enum {
    ID_PILL_LOCK = 10, ID_PILL_SETUP, ID_PILL_NEW, ID_PILL_PAIR,
    ID_PREV, ID_NEXT,
    ID_TILE = 100,
};

#define HOME_PLUGIN_MAX 8

static struct {
    int      page;                  /* shown page of tiles               */
    int      per;                   /* tiles per page, from the last draw */
    int      cols;                  /* tile columns, from the last draw  */
    int      total;                 /* tiles in all, from the last draw  */
    ui_id_t  saved_focus;           /* focus saved when another screen opens */
    bool     kbd_bonded;            /* gates the "Pair" pill             */
    uint8_t  kon_idx;               /* Konami sequence progress          */
    uint64_t next_refresh;          /* next live-status re-render        */
    uint64_t poweroff_until;        /* POWEROFF: when the collapse ends  */
} s_home;

static const char *TAG = "app_home";

/* Keystore presence snapshot, refreshed on enter_home(): render_home runs
 * ~10 Hz and an ABSENT store would stat the filesystem every frame. Store
 * create/remove always route through other screens, so this stays true. */
static bool s_ks_present;

/* Visible plugin tiles in display order; they follow the profiles. */
static int plugin_tiles(const home_tile_t *out[])
{
    int n = 0;
    for (int p = 0; p < cyberdeck_plugin_count; p++) {
        const cyberdeck_plugin_t *pl = cyberdeck_plugins[p];
        for (int i = 0; i < pl->n_home_tiles && n < HOME_PLUGIN_MAX; i++)
            if (!pl->home_tiles[i].visible || pl->home_tiles[i].visible())
                out[n++] = &pl->home_tiles[i];
    }
    return n;
}

/* Phone presence: color alone carries the state. Green is near, blue is in
 * range but far, red means gone after enrolling, amber means enroll mode. Returns
 * false when there is nothing to show. */
static bool phone_pen(uint8_t *pen)
{
    if (!app.presence) return false;
    const bool adv = app.presence->enroll_state() == CYBERDECK_ENROLL_ADVERTISING;
    if (!adv && !app.presence->enrolled()) return false;
    const bool ph = !adv && app.presence->present();
    *pen = adv ? OVERLAY_COL_AMBER
         : (ph && app.presence->is_near()) ? OVERLAY_COL_GREEN
         : ph ? OVERLAY_COL_BLUE : OVERLAY_COL_RED;
    return true;
}

static void render_home(uint64_t now)
{
    const int faults = ui_faults();
    ui_slate_t pg = ui_slate_page();
    const home_tile_t *xt[HOME_PLUGIN_MAX];
    const int nx = plugin_tiles(xt);
    const int total = app.profile_count + nx;

    ui_focus_default(app.profile_count > 0 ? (ui_id_t)ID_TILE
                                           : (ui_id_t)ID_PILL_NEW);

    if (s_ks_present)
        ui_pill(&pg.header, ID_PILL_LOCK, "Lock", UI_TONE_CARD, 0, 0);
    ui_pill(&pg.header, ID_PILL_SETUP, "Setup", UI_TONE_CARD, 0, 0);
    ui_pill(&pg.header, ID_PILL_NEW, "+ New", UI_TONE_CARD, 0,
            app.stored_count >= STORAGE_MAX_PROFILES ? UI_PART_OFF : 0);
    if (app.ble && !s_home.kbd_bonded)
        ui_pill(&pg.header, ID_PILL_PAIR, "Pair", UI_TONE_TINT,
                OVERLAY_COL_CYAN, 0);

    if (app.profile_count == 0) {
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_text(ui_rect(pg.body.x + 1, pg.body.y, pg.body.w - 2, 1),
                "No hosts yet. Tap + New, or import from Setup.", UI_MUTED,
                UI_LEFT);
        ui_cut_top(&pg.body, 2);
    }
    const ui_grid_t g = ui_grid(pg.body,
                                ui_cols_for(pg.body.w, 30, pg.m.gutter), 4,
                                pg.m.gutter, 0);
    const int per = ui_grid_capacity(&g);
    const int pages = ui_page_count(total, per);
    s_home.per = per;
    s_home.cols = g.ncols;
    s_home.total = total;
    s_home.page = ui_page_clamp(s_home.page, total, per);

    char note[16];
    snprintf(note, sizeof note, "%d of %d", app.stored_count,
             STORAGE_MAX_PROFILES);
    const ui_rect_t rest = ui_pages(pg.header, ID_PREV, ID_NEXT, s_home.page,
                                    pages);
    ui_title(rest, NULL, "Hosts", note, false);

    /* Phone dot at the right end of the title row, if the title leaves room. */
    uint8_t ppen;
    if (rest.w - (1 + 5 + 3 + ui_len(note)) >= 8 && phone_pen(&ppen)) {
        const int x = rest.x + rest.w - 5, y = ui_centre(rest.y, rest.h, 1);
        ui_pen(ppen);
        ui_putch(x, y, UI_LED_ON, UI_TEXT);
        ui_puts(x + 2, y, "PHN", UI_TEXT | UI_BOLD);
        ui_pen(OVERLAY_COL_DEFAULT);
    }

    const int first = s_home.page * per;
    int shown = 0;
    for (int i = 0; i < per && first + i < total; i++, shown++) {
        const int t = first + i;
        const ui_rect_t cell = ui_grid_cell(&g, i);
        if (t < app.profile_count) {
            const conn_profile_t *p = &app.profiles[t];
            char addr[128], num[12];
            ui_part_t tile;
            host_tile(p, &tile, addr, sizeof addr);
            snprintf(num, sizeof num, "%d", i + 1);
            tile.right = i < 9 ? num : NULL;
            ui_part(cell, (ui_id_t)(ID_TILE + t), &tile, NULL);
        } else {
            const home_tile_t *h = xt[t - app.profile_count];
            ui_part(cell, (ui_id_t)(ID_TILE + t),
                    &(ui_part_t){ .title = h->label, .detail = h->body,
                                  .stripe = h->accent }, NULL);
        }
    }

    /* Plugin strips just above the StatusBar (pacman lives there), when
     * two rows stay free below the tiles. */
    int grid_end = g.area.y;
    if (shown > 0) {
        const ui_rect_t last = ui_grid_cell(&g, shown - 1);
        grid_end = last.y + last.h;
    }
    if (pg.status.y - grid_end >= 2)
        for (int i = 0; i < cyberdeck_plugin_count; i++)
            if (cyberdeck_plugins[i]->home_strip)
                cyberdeck_plugins[i]->home_strip(ui_rows() - 2, now);

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

static void home_enter(intptr_t arg, uint64_t now)
{
    (void)arg;
    /* kbd_bonded gates the "Pair" pill; s_ks_present gates "Lock".
     * Landing here counts as activity: it keeps a drop/provisioning toast
     * alive before the rain paints over it. */
    s_home.kbd_bonded = ble_has_bond();
    s_ks_present = keystore_state() != KEYSTORE_ABSENT;
    s_home.next_refresh = 0;
    s_home.page = 0;
    s_home.saved_focus = 0;
    session_guard_activity(now);
}

/* Revealed by a pop (host page, editor, menu): keep the page and focus. */
static void home_resume(intptr_t arg, uint64_t now)
{
    const ui_id_t f = s_home.saved_focus;
    const int page = s_home.page;   /* render clamps it to the new count */
    home_enter(arg, now);
    s_home.page = page;
    if (f < ID_TILE || f - ID_TILE < app.profile_count) ui_focus_set(f);
}

static void home_exit(uint64_t now)
{
    (void)now;
    s_home.saved_focus = ui_focus_get();
}

void enter_home(uint64_t now)
{
    nav_reset(SCR_HOME, 0, now);
}

void connect_checked(int idx, uint64_t now)
{
    if (idx < 0 || idx >= app.profile_count) return;
    if (!wifi_manager_is_connected()) {
        toast(now, "wifi not connected yet");
        nav_invalidate();
        return;
    }
    start_connect(idx, now, now);
}

/* On session teardown, poweroff_enter hides the overlay. This lets the
 * CRT collapse play over the last live terminal frame; HOME appears
 * only once it finishes. Toasts set by the caller survive — they are
 * state, rendered when HOME appears. */
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
    if (now >= s_home.next_refresh) {
        s_home.next_refresh = now + ANIM_PERIOD_MS;   /* animation cadence */
        nav_invalidate();   /* live phone dot, pressed flash */
    }
}

static void poweroff_tick(uint64_t now)
{
    /* Collapse finished (or was cut short by input) — bring HOME up. */
    if (now >= s_home.poweroff_until) enter_home(now);
}

static void home_lock(uint64_t now)
{
    keystore_lock();                /* panic: wipe MK, raise the gate */
    app_creds_wipe();               /* ...and the hydrated copies */
    unlock_open_gate(now);
}

static void home_new(uint64_t now)
{
    if (app.stored_count >= STORAGE_MAX_PROFILES) {
        toast(now, "profile list full");
        nav_invalidate();
        return;
    }
    enter_profile(now, -1);
}

/* Step the page; focus lands on the first tile of the new page. */
static void home_flip(int dir)
{
    const home_tile_t *xt[HOME_PLUGIN_MAX];
    const int total = app.profile_count + plugin_tiles(xt);
    const int page = s_home.page + dir;
    if (page < 0 || s_home.per <= 0 || page * s_home.per >= total) return;
    s_home.page = page;
    ui_focus_set((ui_id_t)(ID_TILE + page * s_home.per));
    nav_invalidate();
}

static void home_press(ui_id_t id, uint64_t now)
{
    switch (id) {
    case ID_PILL_LOCK:  home_lock(now);              return;
    case ID_PILL_SETUP: menu_open_config(now);       return;
    case ID_PILL_NEW:   home_new(now);               return;
    case ID_PILL_PAIR:  enter_pairing(now);          return;
    case ID_PREV:       home_flip(-1);               return;
    case ID_NEXT:       home_flip(1);                return;
    default: break;
    }
    if (id < ID_TILE) return;
    const int t = id - ID_TILE;
    if (t < app.profile_count) {
        connect_checked(t, now);
        return;
    }
    const home_tile_t *xt[HOME_PLUGIN_MAX];
    const int i = t - app.profile_count;
    if (i < plugin_tiles(xt) && xt[i]->activate) xt[i]->activate(now);
}

/* A hold opens the host page. Anywhere that is no region it opens
 * pairing, the route that works without a keyboard. */
static void home_hold(ui_id_t id, uint64_t now)
{
    if (id >= ID_TILE) {
        if (id - ID_TILE < app.profile_count) enter_host(now, id - ID_TILE);
    } else if (!id && app.ble) {
        enter_pairing(now);
    }
}

/* Konami progress rides along invisibly; EVERY arrow still navigates.
 * On mismatch, honor the overlapping UP-UP prefix (an extra leading UP
 * must not break the code). */
static void home_konami(ui_key_t k, uint64_t now)
{
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
}

/* Phone presence is a prototype. P starts or cancels enroll mode when the
 * deck holds no phone. With one phone enrolled, P shows status. A second
 * P within 2 s forgets the identity and bond. It then drops straight back
 * into enroll: the clean re-pair gesture. The phone side still needs its
 * own "Forget This Device". */
static void home_phone_key(uint64_t now)
{
    static uint64_t s_p_last;
    const cyberdeck_presence_ops_t *pr = app.presence;
    if (pr->enroll_state() == CYBERDECK_ENROLL_ADVERTISING) {
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
        toast(now, "phone here (%d dBm) - P again to re-pair", pr->rssi());
    } else {
        const uint32_t age = pr->age_ms();
        if (age == UINT32_MAX)
            toast(now, "phone not seen yet - P again to re-pair");
        else
            toast(now, "phone away (%us) - P again to re-pair",
                  (unsigned)(age / 1000));
    }
    s_p_last = now;
}

/* Letter and digit keys. Returns true when @p ch was one of them. */
static bool home_char(char ch, uint64_t now)
{
    if (ch >= '1' && ch <= '9') {
        /* A digit is the Nth tile of the shown page. */
        const int i = ch - '1';
        if (i < s_home.per) connect_checked(s_home.page * s_home.per + i, now);
        return true;
    }
    switch (ch) {
    case 'b': case 'B': enter_pairing(now);                      return true;
    case 'n': case 'N': home_new(now);                           return true;
    case 'r': case 'R': load_profiles(); nav_invalidate();       return true;
    case 'w': case 'W': kick_wifi(); nav_invalidate();           return true;
    case 'l': case 'L':
        if (!s_ks_present) return true;
        home_lock(now);                 /* keyboard panic button */
        return true;
    case 'p': case 'P':
        if (!app.presence) return true;
        home_phone_key(now);
        nav_invalidate();
        return true;
    default:
        return false;
    }
}

static void home_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                       uint64_t now)
{
    if (k == K_CHAR) {
        if (home_char(ch, now)) return;
        if (ch != ' ') return;
        k = K_HOLD;                     /* Space = hold on the focused part */
    }
    if (k == K_SCROLL_UP || k == K_SCROLL_DOWN) {
        home_flip(k == K_SCROLL_UP ? -1 : 1);
        return;
    }
    if (k == K_UP || k == K_DOWN || k == K_LEFT || k == K_RIGHT)
        home_konami(k, now);

    /* An arrow at the edge of a page turns it (ui_page_step). */
    const ui_id_t f = ui_focus_get();
    if (f >= ID_TILE && ui_focused(f)) {
        const int to = ui_page_step(k, f - ID_TILE, s_home.total, s_home.cols,
                                    s_home.per);
        if (to >= 0) {
            s_home.page = to / s_home.per;
            ui_focus_set((ui_id_t)(ID_TILE + to));
            nav_invalidate();
            return;
        }
    }

    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind == UI_ACT_NONE) return;
    nav_invalidate();
    if (a.kind == UI_ACT_PRESS)     home_press(a.id, now);
    else if (a.kind == UI_ACT_HOLD) home_hold(a.id, now);
}

static void poweroff_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                           uint64_t now)
{
    (void)ev; (void)k; (void)ch;
    /* Any input skips the rest of the power-off animation. */
    enter_home(now);
}

const nav_screen_t home_screen = {
    .name = "home", .enter = home_enter, .resume = home_resume,
    .exit = home_exit, .tick = home_tick, .input = home_input,
    .render = render_home, .chrome = NAV_CHROME_FULL,
};

/* No render: the collapse fx plays over the dead terminal frame. */
const nav_screen_t poweroff_screen = {
    .name = "poweroff", .enter = poweroff_enter, .tick = poweroff_tick,
    .input = poweroff_input, .chrome = NAV_CHROME_NONE,
};
