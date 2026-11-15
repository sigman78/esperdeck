/*
 * app_statusbar.c — the StatusBar chip engine (docs/status-bar.md).
 *
 * A chip is a label plus a value field, drawn as a run of cells. Each
 * cell's state (ABSENT/OFF/ON/BUSY/ALERT) picks its look, so a lit lock
 * sits inside an otherwise plain chip. This file polls a static table
 * every frame. The kit's ui_status draws the row. The chip types stay
 * file-internal until the plugin seam (step 5).
 */

#include "app_widgets.h"
#include "app_screens.h"
#include "wifi_manager.h"
#include "vterm.h"

#include <stdio.h>
#include <string.h>

#define SB_LADDER         4    /* NET signal ladder, cells              */
#define SB_RSSI_FRAMES    10   /* RSSI snapshot cadence, ~1 s at 10 fps  */
#define SB_MAX_CHIPS      3

/* HOME's link-quality buckets, dBm. */
#define SB_RSSI_GOOD   (-55)
#define SB_RSSI_FAIR   (-67)
#define SB_RSSI_WEAK   (-78)

/* Lower-eighth block ramp, one rung per ladder cell: the lit run reads
 * as a rising staircase. */
#define SB_STAIR_1     0x2582u
#define SB_STAIR_2     0x2584u
#define SB_STAIR_3     0x2586u
#define SB_STAIR_4     0x2588u

typedef struct {
    const char *id;        /* stable key for the plugin seam            */
    uint8_t     accent;    /* default ON accent                         */
    int       (*poll)(ui_sb_cell_t cell[UI_SB_CELLS]);  /* fill, return count */
    /* Both NULL for a chip that never announces. state returns a UI_SB_*
     * value. It sets @p quiet when a loss is only a scan. detail writes
     * the text of an UI_ANN_* edge. */
    uint8_t   (*state)(bool *quiet);
    void      (*detail)(int kind, char *buf, size_t sz);
} chip_desc_t;

/* The chip's name plus the separator cell that opens its value field.
 * Label cells are text: they take the state's look, never its glyph. */
static int sb_label(ui_sb_cell_t cell[UI_SB_CELLS], const char *s, uint8_t st)
{
    int n = 0;
    for (; *s && n < UI_SB_CELLS - 1; s++, n++) {
        cell[n].state = st;
        cell[n].cp    = (uint16_t)(unsigned char)*s;
        cell[n].keep  = true;
    }
    cell[n].state = st;
    cell[n].cp    = ' ';
    cell[n].keep  = true;
    return n + 1;
}

int net_rssi(void)
{
    /* RSSI moves over seconds; taking the WiFi driver's lock at the render
     * cadence is what this cache avoids. The setup hub reads it too. */
    static bool     fresh = false;
    static uint32_t frame = 0;
    static int      rssi  = 0;
    if (!fresh || app.anim_frame - frame >= SB_RSSI_FRAMES) {
        fresh = true;
        frame = app.anim_frame;
        rssi  = wifi_manager_get_rssi();
    }
    return rssi;
}

static uint8_t net_state(bool *quiet)
{
    if (quiet) *quiet = false;
    switch (wifi_manager_get_state()) {
    case WIFI_MGR_CONNECTED:  return UI_SB_ON;
    case WIFI_MGR_CONNECTING:
    case WIFI_MGR_LOST:       return UI_SB_BUSY;
    case WIFI_MGR_FAILED:     return UI_SB_ALERT;
    default:                  return UI_SB_OFF;
    }
}

static void net_detail(int kind, char *buf, size_t sz)
{
    if (kind == UI_ANN_UP) {
        const char *ssid = wifi_manager_get_ssid();
        const char *ip   = wifi_manager_get_ip();
        if (!ip) ip = "";
        if (ssid && ssid[0]) snprintf(buf, sz, "%.32s %.15s", ssid, ip);
        else                 snprintf(buf, sz, "%.15s", ip);
    } else {
        snprintf(buf, sz, "%s", kind == UI_ANN_FAIL ? "join failed"
                                                    : "link lost");
    }
}

static int poll_net(ui_sb_cell_t cell[UI_SB_CELLS])
{
    const int rssi = net_rssi();

    const uint8_t st = net_state(NULL);
    int n = sb_label(cell, "W", st);

    /* Rung i is lit once quality reaches level i+1. Unlit rungs stay
     * blank. On a focus chip the wash is the chip. A blank cell already
     * reads as the empty part of the gauge. get_rssi reads 0 while the
     * link is down, which would light every rung. */
    static const uint16_t STAIR[SB_LADDER] = {
        SB_STAIR_1, SB_STAIR_2, SB_STAIR_3, SB_STAIR_4,
    };
    const int level = st != UI_SB_ON      ? 0
                    : rssi > SB_RSSI_GOOD ? 4
                    : rssi > SB_RSSI_FAIR ? 3
                    : rssi > SB_RSSI_WEAK ? 2 : 1;
    for (int i = 0; i < SB_LADDER; i++, n++) {
        cell[n].state = st;
        cell[n].cp    = i < level ? STAIR[i] : ' ';
        /* The first rung carries the spinner and the alert mark: BUSY and
         * ALERT mark the field once, not four times. */
        cell[n].keep  = i > 0;
    }
    return n;
}

/* Lock indicators, one cell each. The backend reports exactly these two;
 * a _Static_assert in main.c pins them to the BLE bits. The field is two
 * cells wide — there is no third lock to show. */
static const struct { char c; uint8_t bit; } KBD_LOCKS[] = {
    { 'C', CYBERDECK_KBD_LOCK_CAPS },
    { 'N', CYBERDECK_KBD_LOCK_NUM  },
};

/* A scan is no loss: the pairing page narrates it. A keyboard on its way
 * up shows as down: a still grey chip, no spinner. */
static uint8_t kbd_state(bool *quiet)
{
    if (quiet) *quiet = false;
    if (!app.ble || !app.ble->get_state) return UI_SB_ABSENT;
    const cyberdeck_ble_state_t b = app.ble->get_state();
    if (quiet) *quiet = b == CYBERDECK_BLE_PAIRING_SCAN;
    switch (b) {
    case CYBERDECK_BLE_CONNECTED:    return UI_SB_ON;
    default:                         return UI_SB_OFF;
    }
}

static void kbd_detail(int kind, char *buf, size_t sz)
{
    if (kind == UI_ANN_UP) {
        const char *name = app.ble && app.ble->get_name ? app.ble->get_name()
                                                        : NULL;
        snprintf(buf, sz, "%.54s", name && name[0] ? name : "connected");
    } else {
        snprintf(buf, sz, "disconnected");
    }
}

static int poll_kbd(ui_sb_cell_t cell[UI_SB_CELLS])
{
    if (!app.ble || !app.ble->get_state) return 0;

    const uint8_t st = kbd_state(NULL);
    int n = sb_label(cell, "KBD", st);

    /* A lock has no referent while the keyboard is down. Off ON the field
     * goes blank. Its first cell carries the spinner or the alert mark, as
     * the NET ladder does. An unlit lock takes the chip's own well, not
     * the OFF blue: it reads as part of the chip. */
    const bool    up    = st == UI_SB_ON;
    const uint8_t locks = (up && app.ble->get_locks) ? app.ble->get_locks() : 0;
    for (int i = 0; i < NELEM(KBD_LOCKS) && n < UI_SB_CELLS; i++, n++) {
        const bool lit = (locks & KBD_LOCKS[i].bit) != 0;
        cell[n].state  = lit ? UI_SB_ON : (up ? UI_SB_OFF : st);
        cell[n].accent = !up ? 0 : lit ? OVERLAY_COL_AMBER : OVERLAY_COL_CYAN;
        cell[n].cp     = up ? (uint16_t)(unsigned char)KBD_LOCKS[i].c : ' ';
        cell[n].keep   = up || i > 0;
    }
    return n;
}

/* TEMPORARY soak diagnostic (2026-09): the vterm ?2026 watchdog fired.
 * Absent until the first incident; then a red chip with the count (9+).
 * Remove once we know what drops the ESU. */
static int poll_sync(ui_sb_cell_t cell[UI_SB_CELLS])
{
    const uint32_t n = vterm_sync_timeouts();
    if (n == 0) return 0;
    const int k = sb_label(cell, "SYNC", UI_SB_ALERT);
    cell[k].state = UI_SB_ALERT;
    cell[k].cp    = (uint16_t)(n > 9 ? '9' : '0' + n);
    cell[k].keep  = true;
    return k + 1;
}

static const chip_desc_t CHIPS[] = {
    { .id = "net",  .accent = OVERLAY_COL_GREEN, .poll = poll_net,
      .state = net_state, .detail = net_detail },
    { .id = "kbd",  .accent = OVERLAY_COL_CYAN,  .poll = poll_kbd,
      .state = kbd_state, .detail = kbd_detail },
    { .id = "sync", .accent = OVERLAY_COL_AMBER, .poll = poll_sync },
};

_Static_assert(sizeof(CHIPS) / sizeof(CHIPS[0]) <= SB_MAX_CHIPS,
               "the status row holds SB_MAX_CHIPS chips");

/* Polls the table into the chips the kit draws; returns how many. A
 * table entry that draws nothing gets slot -1 in @p slot. */
static int sb_poll(ui_sb_chip_t chips[SB_MAX_CHIPS], int slot[SB_MAX_CHIPS])
{
    int count = 0;
    for (int i = 0; i < NELEM(CHIPS); i++) {
        ui_sb_chip_t *c = &chips[count];
        memset(c, 0, sizeof(*c));
        slot[i] = -1;
        const int n = CHIPS[i].poll(c->cell);
        if (n <= 0 || c->cell[0].state == UI_SB_ABSENT) continue;
        c->accent = CHIPS[i].accent;
        c->n      = (uint8_t)n;
        slot[i]   = count++;
    }
    return count;
}

/* The chip announcements. A chip that moves queues a detail text. The
 * tick below times it. The render only reads it. */
static ui_ann_t s_ann;
static bool     s_primed;               /* a screen with a bar has been up   */
static uint8_t  s_prev[SB_MAX_CHIPS];   /* last UI_SB_* state per table entry */
static uint32_t s_sig;                  /* what the last tick saw drawn      */
static uint32_t s_blink;                /* last frame drawn for a busy chip  */

/* What the row draws apart from the note: the chips and the clock. */
static uint32_t chips_sig(const ui_sb_chip_t *chips, int n, const char *clk)
{
    uint32_t h = 2166136261u;
#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    for (; *clk; clk++) MIX(*clk);
    for (int i = 0; i < n; i++) {
        MIX(chips[i].n);
        for (int k = 0; k < chips[i].n; k++) {
            MIX(chips[i].cell[k].state);
            MIX(chips[i].cell[k].cp);
            MIX(chips[i].cell[k].accent);
        }
    }
#undef MIX
    return h;
}

static bool chips_busy(const ui_sb_chip_t *chips, int n)
{
    for (int i = 0; i < n; i++)
        for (int k = 0; k < chips[i].n; k++)
            if (chips[i].cell[k].state == UI_SB_BUSY) return true;
    return false;
}

/* A session hides the bar. Its chip edges go to a toast. */
static void session_news(uint64_t now, int chip, int kind)
{
    if (kind == UI_ANN_NONE) return;
    if (kind == UI_ANN_UP) {
        char text[UI_ANN_TEXT];
        CHIPS[chip].detail(kind, text, sizeof(text));
        const char *what = chip == 0 ? "wifi" : "keyboard";
        if (strcmp(text, "connected") == 0) toast(now, "%s connected", what);
        else toast(now, "%s connected - %s", what, text);
        app.toast_ok = true;
        return;
    }
    if (chip == 0 && kind == UI_ANN_FAIL)
        toast_for(now, ERR_TOAST_MS, "wifi connect failed (retrying)");
    else if (chip == 0)
        toast(now, "wifi disconnected");
    else
        toast(now, "keyboard disconnected");
}

/* Two more facts the hidden bar would show in a session: a lock key
 * changed, and the link got weak. The state tracks on every screen. A
 * toast goes out only in a session. A weak link speaks once, then waits
 * until the signal is fair again. */
static void session_facts(uint64_t now, bool session)
{
    static const char *const LOCK_NAME[NELEM(KBD_LOCKS)] = { "caps", "num" };
    static bool    s_kbd_up, s_weak;
    static uint8_t s_locks;

    const bool    up    = kbd_state(NULL) == UI_SB_ON;
    const uint8_t locks = (up && app.ble->get_locks) ? app.ble->get_locks() : 0;
    if (session && up && s_kbd_up) {
        for (int i = 0; i < NELEM(KBD_LOCKS); i++)
            if ((locks ^ s_locks) & KBD_LOCKS[i].bit)
                toast(now, "%s lock %s", LOCK_NAME[i],
                      locks & KBD_LOCKS[i].bit ? "on" : "off");
    }
    s_kbd_up = up;
    s_locks  = locks;

    const int rssi = net_state(NULL) == UI_SB_ON ? net_rssi() : 0;
    if (rssi == 0 || rssi > SB_RSSI_FAIR) {
        s_weak = false;
    } else if (rssi <= SB_RSSI_WEAK && !s_weak) {
        s_weak = true;
        if (session) toast(now, "wifi signal weak (%d dBm)", rssi);
    }
}

void status_tick(uint64_t now)
{
    if (app.toast[0] && now >= app.toast_until) {
        app.toast[0]   = '\0';
        app.toast_wifi = false;
        nav_invalidate();
    }

    const bool bar = nav_has_bar();
    if (!s_primed && !bar) return;
    const bool first = !s_primed;
    s_primed = true;

    for (int i = 0; i < NELEM(CHIPS); i++) {
        if (!CHIPS[i].state) continue;
        bool quiet = false;
        const uint8_t cur = CHIPS[i].state(&quiet);
        const int kind = ui_ann_edge(s_prev[i], cur, first, quiet);
        s_prev[i] = cur;
        if (kind == UI_ANN_NONE) continue;
        if (bar) {
            char text[UI_ANN_TEXT];
            CHIPS[i].detail(kind, text, sizeof(text));
            ui_ann_post(&s_ann, i, text, now);
        } else if (nav_current() == SCR_SESSION) {
            session_news(now, i, kind);
        }
    }
    session_facts(now, !bar && nav_current() == SCR_SESSION);

    ui_sb_chip_t chips[SB_MAX_CHIPS];
    int          slot[SB_MAX_CHIPS];
    char         clk[10] = "";
    const int nchips = sb_poll(chips, slot);
    if (bar && !clock_str(clk, sizeof(clk))) clk[0] = '\0';
    const uint32_t sig = chips_sig(chips, nchips, clk);
    const bool moved = ui_ann_step(&s_ann, now);
    if ((moved || sig != s_sig) && bar) nav_invalidate();
    s_sig = sig;

    /* A busy chip blinks and spins. A still page gets a frame for it. */
    if (bar && app.anim_frame != s_blink && chips_busy(chips, nchips)) {
        s_blink = app.anim_frame;
        nav_invalidate();
    }
}

bool status_idle(void)
{
    return s_primed ? !ui_ann_busy(&s_ann) : !nav_has_bar();
}

void ui_statusbar(uint64_t now)
{
    ui_sb_chip_t chips[SB_MAX_CHIPS];
    int          slot[SB_MAX_CHIPS];
    char         clk[10];
    ui_status_t  s = { .chips = chips, .frame = app.anim_frame };

    s.nchips = sb_poll(chips, slot);
    if (clock_str(clk, sizeof(clk))) s.clock = clk;

    const int live = ui_ann_chip(&s_ann);
    if (live >= 0 && slot[live] >= 0 && ui_ann_width(&s_ann) > 0) {
        s.xchip = slot[live];
        s.xtext = ui_ann_text(&s_ann);
        s.xw    = ui_ann_width(&s_ann);
    }

    /* No keystore-lock chip, by user call 2026-08-27. A locked deck shows
     * the PIN pad, so the state is self-evident. */
    if (app.toast[0] && now < app.toast_until) {
        s.note = app.toast;
        s.tone = app.toast_ok ? UI_NOTE_GOOD : UI_NOTE_PLAIN;
    }
    ui_status(ui_rect(0, ui_rows() - 1, ui_cols(), 1), &s);
}
