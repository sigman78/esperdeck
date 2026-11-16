/*
 * app_pairing.c — the keyboard pairing page (SCR_PAIRING).
 * Found keyboards are tiles. Forget bonds asks on the question page.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "ssh_client.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

#define PAIR_MAX  STORAGE_BLE_MAX

enum { ID_DEV = 1, ID_FORGET = 20, ID_PREV, ID_NEXT };  /* ID_DEV + 0..PAIR_MAX-1 */

static const char *TAG = "app_pairing";

static struct {
    ble_device_info_t devs[PAIR_MAX];
    int      ndevs;
    int      page, per;         /* tile page; tiles per page from render */
    uint64_t last_poll;
    uint64_t last_activity;
    ui_id_t  saved_focus;       /* focus kept while the question is up */
} s_pair;

#define PAIR_TIMEOUT_MS  30000
#define PAIR_COUNTDOWN_MS 10000
#define PAIR_POLL_MS     250

/* The scan self-dismisses after PAIR_TIMEOUT_MS of inactivity. The last
 * seconds show as a countdown. Returns the seconds left, or 0. */
static unsigned countdown_s(uint64_t now)
{
    const uint64_t idle = now - s_pair.last_activity;
    if (idle > PAIR_TIMEOUT_MS || idle <= PAIR_TIMEOUT_MS - PAIR_COUNTDOWN_MS)
        return 0;
    return (unsigned)((PAIR_TIMEOUT_MS - idle + 999) / 1000);
}

static void render_pairing(uint64_t now)
{
    const int faults = ui_faults();
    ui_slate_t pg = ui_slate_page();
    const ui_rect_t body = pg.body;
    char spin[8], text[32];

    /* Forget bonds and its hint hold the bottom rows, so the buttons stay
     * put while keyboards come and go. */
    ui_grid_t g = ui_grid(ui_rect(body.x, body.y + 1, body.w, body.h - 5),
                          ui_cols_for(body.w, 30, pg.m.gutter), 4,
                          pg.m.gutter, 0);
    const int cap = ui_grid_capacity(&g);
    s_pair.per = cap;
    s_pair.page = ui_page_clamp(s_pair.page, s_pair.ndevs, cap);
    const int first = s_pair.page * cap;
    const int shown = s_pair.ndevs - first < cap ? s_pair.ndevs - first : cap;

    ui_focus_default((ui_id_t)(ID_DEV + first));
    ui_title(ui_pages(pg.header, ID_PREV, ID_NEXT, s_pair.page,
                      ui_page_count(s_pair.ndevs, cap)),
             "Keyboard", "Pair keyboard", NULL, true);
    ui_utf8_put(spin, spinner_glyph(app.anim_frame));
    snprintf(text, sizeof text, "%d found. Tap yours.", s_pair.ndevs);
    ui_state(ui_rect(body.x, body.y, body.w, 1),
             s_pair.ndevs ? UI_S_CHECK : spin,
             s_pair.ndevs ? OVERLAY_COL_GREEN : OVERLAY_COL_AMBER,
             s_pair.ndevs ? text : "Scanning for keyboards");
    const unsigned left = countdown_s(now);
    if (left) {
        snprintf(text, sizeof text, "closes in %u s", left);
        ui_pen(OVERLAY_COL_AMBER);
        ui_text(ui_rect(body.x, body.y, body.w - 2, 1), text, UI_TEXT,
                UI_RIGHT);
        ui_pen(OVERLAY_COL_DEFAULT);
    }
    for (int i = 0; i < shown; i++)
        ui_part(ui_grid_cell(&g, i), (ui_id_t)(ID_DEV + first + i),
                &(ui_part_t){ .title = s_pair.devs[first + i].name,
                              .detail = s_pair.devs[first + i].addr_type
                                            ? "random address"
                                            : "public address",
                              .stripe = OVERLAY_COL_GREEN }, NULL);
    ui_text(ui_rect(body.x + 2, body.y + body.h - 4, body.w - 2, 1),
            "Put the keyboard in pairing mode.", UI_MUTED, UI_LEFT);
    ui_push(ui_rect(body.x, body.y + body.h - 3, ui_push_w("Forget bonds"), 3),
            ID_FORGET, "Forget bonds", UI_TONE_TINT, OVERLAY_COL_RED, 0);

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

static void scan_restart(uint64_t now)
{
    app.ble->enter_pairing();
    s_pair.ndevs = 0;
    s_pair.page = 0;
    s_pair.last_poll = 0;
    s_pair.last_activity = now;
}

static void pairing_enter(intptr_t arg, uint64_t now)
{
    (void)arg;
    s_pair.saved_focus = 0;
    scan_restart(now);
}

/* Back from the question: leaving the page stopped the scan, so start it
 * again. The idle timer restarts. The focus returns where it was. */
static void pairing_resume(intptr_t arg, uint64_t now)
{
    (void)arg;
    scan_restart(now);
    if (s_pair.saved_focus == ID_FORGET) ui_focus_set(ID_FORGET);
}

static void pairing_exit(uint64_t now)
{
    (void)now;
    s_pair.saved_focus = ui_focus_get();
    if (app.ble && app.ble->exit_pairing)
        app.ble->exit_pairing();
}

void enter_pairing(uint64_t now)
{
    if (!app.ble || !app.ble->enter_pairing) return;
    nav_push(SCR_PAIRING, 0, now);
}

/* The question page popped us, which restarted the scan. Forget, then
 * restart it once more so it starts from clean bonds. */
static void forget_yes(intptr_t arg, uint64_t now)
{
    (void)arg;
    if (app.ble && app.ble->forget) app.ble->forget();
    if (app.ble && app.ble->exit_pairing) app.ble->exit_pairing();
    scan_restart(now);
    toast(now, "bonds cleared - re-scanning");
    nav_invalidate();
}

static void ask_forget(uint64_t now)
{
    ask_open(&(ask_t){
        .title = "Forget every paired keyboard?",
        .line1 = "Every bond is deleted from the deck.",
        .line2 = "Pair again from this page.",
        .verb = "Forget", .accent = OVERLAY_COL_RED,
        .yes = forget_yes,
    }, now);
}

static void pairing_tick(uint64_t now)
{
    if (now - s_pair.last_activity > PAIR_TIMEOUT_MS) {
        toast(now, "pairing timed out");
        nav_pop(now);
        return;
    }
    if (now >= app.next_anim) {          /* advance the spinner and countdown */
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
    if (now - s_pair.last_poll >= PAIR_POLL_MS && app.ble) {
        s_pair.last_poll = now;
        ble_device_info_t fresh[PAIR_MAX];
        int n = app.ble->get_scan_results(fresh, PAIR_MAX);
        if (n != s_pair.ndevs ||
            memcmp(fresh, s_pair.devs, (size_t)n * sizeof(fresh[0])) != 0) {
            /* The first keyboard takes the focus, unless it moved to Forget. */
            if (s_pair.ndevs == 0 && n > 0 && ui_focus_get() == ID_FORGET)
                ui_focus_set(ID_DEV);
            memcpy(s_pair.devs, fresh, sizeof(fresh));
            s_pair.ndevs = n;
            s_pair.last_activity = now;   /* results still arriving */
            nav_invalidate();
        }
    }
}

/* Step the tile page; focus lands on the first tile of the new page. */
static void pair_flip(int dir)
{
    const int page = s_pair.page + dir;
    if (page < 0 || s_pair.per <= 0 || page * s_pair.per >= s_pair.ndevs)
        return;
    s_pair.page = page;
    ui_focus_set((ui_id_t)(ID_DEV + page * s_pair.per));
    nav_invalidate();
}

static void pairing_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                          uint64_t now)
{
    (void)ch;
    s_pair.last_activity = now;
    if (k == K_ESC) {
        nav_pop(now);
        return;
    }
    if (k == K_SCROLL_UP || k == K_SCROLL_DOWN) {
        pair_flip(k == K_SCROLL_UP ? -1 : 1);
        return;
    }
    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind == UI_ACT_NONE) return;
    nav_invalidate();
    if (a.kind != UI_ACT_PRESS) return;
    if (a.id == UI_ID_BACK) {
        nav_pop(now);
    } else if (a.id == ID_FORGET) {
        ask_forget(now);
    } else if (a.id == ID_PREV || a.id == ID_NEXT) {
        pair_flip(a.id == ID_NEXT ? 1 : -1);
    } else if (a.id >= ID_DEV && a.id < ID_DEV + s_pair.ndevs &&
               a.id < ID_DEV + PAIR_MAX && app.ble) {
        const ble_device_info_t *d = &s_pair.devs[a.id - ID_DEV];
        app.ble->select_device(d->addr, d->addr_type);
        toast(now, "pairing %.32s...", d->name);
        nav_pop(now);
    }
}

const nav_screen_t pairing_screen = {
    .name = "pairing", .enter = pairing_enter, .resume = pairing_resume,
    .exit = pairing_exit, .tick = pairing_tick, .input = pairing_input,
    .render = render_pairing, .chrome = NAV_CHROME_FULL,
};
