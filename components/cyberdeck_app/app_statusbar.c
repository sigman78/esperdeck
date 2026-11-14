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
#include "wifi_manager.h"
#include "vterm.h"

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

static int poll_net(ui_sb_cell_t cell[UI_SB_CELLS])
{
    const int rssi = net_rssi();

    uint8_t st;
    switch (wifi_manager_get_state()) {
    case WIFI_MGR_CONNECTED:  st = UI_SB_ON;    break;
    case WIFI_MGR_CONNECTING:
    case WIFI_MGR_LOST:       st = UI_SB_BUSY;  break;
    case WIFI_MGR_FAILED:     st = UI_SB_ALERT; break;
    default:                  st = UI_SB_OFF;   break;
    }
    int n = sb_label(cell, "NET", st);

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

static int poll_kbd(ui_sb_cell_t cell[UI_SB_CELLS])
{
    if (!app.ble || !app.ble->get_state) return 0;

    uint8_t st;
    switch (app.ble->get_state()) {
    case CYBERDECK_BLE_CONNECTED:    st = UI_SB_ON;   break;
    case CYBERDECK_BLE_CONNECTING:
    case CYBERDECK_BLE_PAIRING_SCAN:
    case CYBERDECK_BLE_RECONNECT:    st = UI_SB_BUSY; break;
    default:                         st = UI_SB_OFF;  break;
    }
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
    { "net",  OVERLAY_COL_GREEN, poll_net  },
    { "kbd",  OVERLAY_COL_CYAN,  poll_kbd  },
    { "sync", OVERLAY_COL_AMBER, poll_sync },
};

_Static_assert(sizeof(CHIPS) / sizeof(CHIPS[0]) <= SB_MAX_CHIPS,
               "the status row holds SB_MAX_CHIPS chips");

/* Polls the table into the chips the kit draws; returns how many. */
static int sb_poll(ui_sb_chip_t chips[SB_MAX_CHIPS])
{
    int count = 0;
    for (int i = 0; i < NELEM(CHIPS); i++) {
        ui_sb_chip_t *c = &chips[count];
        memset(c, 0, sizeof(*c));
        const int n = CHIPS[i].poll(c->cell);
        if (n <= 0 || c->cell[0].state == UI_SB_ABSENT) continue;
        c->accent = CHIPS[i].accent;
        c->n      = (uint8_t)n;
        count++;
    }
    return count;
}

void ui_statusbar(uint64_t now)
{
    ui_sb_chip_t chips[SB_MAX_CHIPS];
    char         clk[10];
    ui_status_t  s = { .chips = chips, .frame = app.anim_frame };

    s.nchips = sb_poll(chips);
    if (clock_str(clk, sizeof(clk))) s.clock = clk;

    /* No keystore-lock chip, by user call 2026-08-27. A locked deck shows
     * the PIN pad, so the state is self-evident. */
    if (app.toast[0] && now < app.toast_until) {
        s.note = app.toast;
        s.tone = app.toast_ok ? UI_NOTE_GOOD : UI_NOTE_PLAIN;
    }
    ui_status(ui_rect(0, ui_rows() - 1, ui_cols(), 1), &s);
}
