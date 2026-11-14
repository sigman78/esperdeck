/*
 * app_statusbar.c — the StatusBar chip engine (docs/status-bar.md).
 *
 * A chip is a label plus a value field, drawn as a run of cells. Each
 * cell's state (ABSENT/OFF/ON/BUSY/ALERT) picks its style+pen pair, so a
 * lit lock sits inside an otherwise plain chip. The walk polls a static
 * table every frame. The chip types stay file-internal until the plugin
 * seam (step 5).
 */

#include "app_widgets.h"
#include "wifi_manager.h"
#include "vterm.h"

#include <string.h>

#define SB_LEFT_COL       1    /* first cell of the left cluster        */
#define SB_MAX_CELLS      10   /* label + separator + value field       */
#define SB_LADDER         4    /* NET signal ladder, cells             */
#define SB_CHIP_GAP       1    /* bar cells between adjacent chips      */
#define SB_BLINK_FRAMES   8    /* BUSY blink half-period, ~1 Hz at 10 fps */
#define SB_RSSI_FRAMES    10   /* RSSI snapshot cadence, ~1 s at 10 fps  */
#define SB_TOAST_MIN_GAP  16   /* under this the toast takes the span    */

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
#define SB_ALERT_ICON  0x2718u   /* heavy ballot X */

typedef enum {
    CHIP_ABSENT = 0, CHIP_OFF, CHIP_ON, CHIP_BUSY, CHIP_ALERT
} chip_state_t;

/* One cell of a chip. A chip is a run of these plus the pads around it;
 * per-cell state is what lets a lit lock sit inside a connected chip. */
typedef struct {
    chip_state_t state;
    uint16_t     cp;       /* codepoint for this cell                   */
    uint8_t      accent;   /* accent override; 0 = descriptor's own     */
    bool         keep;     /* never substitute the spinner/alert glyph  */
} chip_cell_t;

typedef struct {
    const char *id;        /* stable key for the plugin seam            */
    uint8_t     accent;    /* default ON accent                         */
    int       (*poll)(chip_cell_t cell[SB_MAX_CELLS]);  /* fill, return count */
} chip_desc_t;

/* The chip's name plus the separator cell that opens its value field.
 * Label cells are text: they take the state's look, never its glyph. */
static int sb_label(chip_cell_t cell[SB_MAX_CELLS], const char *s,
                    chip_state_t st)
{
    int n = 0;
    for (; *s && n < SB_MAX_CELLS - 1; s++, n++) {
        cell[n].state = st;
        cell[n].cp    = (uint16_t)(unsigned char)*s;
        cell[n].keep  = true;
    }
    cell[n].state = st;
    cell[n].cp    = ' ';
    cell[n].keep  = true;
    return n + 1;
}

static int poll_net(chip_cell_t cell[SB_MAX_CELLS])
{
    /* RSSI moves over seconds; taking the WiFi driver's lock at the render
     * cadence is the thing this cache exists to avoid (as on HOME). */
    static bool     fresh = false;
    static uint32_t frame = 0;
    static int      rssi  = 0;
    if (!fresh || app.anim_frame - frame >= SB_RSSI_FRAMES) {
        fresh = true;
        frame = app.anim_frame;
        rssi  = wifi_manager_get_rssi();
    }

    chip_state_t st;
    switch (wifi_manager_get_state()) {
    case WIFI_MGR_CONNECTED:  st = CHIP_ON;    break;
    case WIFI_MGR_CONNECTING:
    case WIFI_MGR_LOST:       st = CHIP_BUSY;  break;
    case WIFI_MGR_FAILED:     st = CHIP_ALERT; break;
    default:                  st = CHIP_OFF;   break;
    }
    int n = sb_label(cell, "NET", st);

    /* Rung i is lit once quality reaches level i+1. Unlit rungs stay
     * blank. On a focus chip the wash is the chip. A blank cell already
     * reads as the empty part of the gauge. get_rssi reads 0 while the
     * link is down, which would light every rung. */
    static const uint16_t STAIR[SB_LADDER] = {
        SB_STAIR_1, SB_STAIR_2, SB_STAIR_3, SB_STAIR_4,
    };
    const int level = st != CHIP_ON      ? 0
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

static int poll_kbd(chip_cell_t cell[SB_MAX_CELLS])
{
    if (!app.ble || !app.ble->get_state) return 0;

    chip_state_t st;
    switch (app.ble->get_state()) {
    case CYBERDECK_BLE_CONNECTED:    st = CHIP_ON;   break;
    case CYBERDECK_BLE_CONNECTING:
    case CYBERDECK_BLE_PAIRING_SCAN:
    case CYBERDECK_BLE_RECONNECT:    st = CHIP_BUSY; break;
    default:                         st = CHIP_OFF;  break;
    }
    int n = sb_label(cell, "KBD", st);

    /* A lock has no referent while the keyboard is down. Off ON the field
     * goes blank. Its first cell carries the spinner or the alert mark, as
     * the NET ladder does. An unlit lock takes the chip's own well, not
     * the OFF blue: it reads as part of the chip. */
    const bool    up    = st == CHIP_ON;
    const uint8_t locks = (up && app.ble->get_locks) ? app.ble->get_locks() : 0;
    for (int i = 0; i < NELEM(KBD_LOCKS) && n < SB_MAX_CELLS; i++, n++) {
        const bool lit = (locks & KBD_LOCKS[i].bit) != 0;
        cell[n].state  = lit ? CHIP_ON : (up ? CHIP_OFF : st);
        cell[n].accent = !up ? 0 : lit ? OVERLAY_COL_AMBER : OVERLAY_COL_CYAN;
        cell[n].cp     = up ? (uint16_t)(unsigned char)KBD_LOCKS[i].c : ' ';
        cell[n].keep   = up || i > 0;
    }
    return n;
}

/* TEMPORARY soak diagnostic (2026-09): the vterm ?2026 watchdog fired.
 * Absent until the first incident; then a red chip with the count (9+).
 * Remove once we know what drops the ESU. */
static int poll_sync(chip_cell_t cell[SB_MAX_CELLS])
{
    const uint32_t n = vterm_sync_timeouts();
    if (n == 0) return 0;
    const int k = sb_label(cell, "SYNC", CHIP_ALERT);
    cell[k].state = CHIP_ALERT;
    cell[k].cp    = (uint16_t)(n > 9 ? '9' : '0' + n);
    cell[k].keep  = true;
    return k + 1;
}

static const chip_desc_t CHIPS[] = {
    { "net",  OVERLAY_COL_GREEN, poll_net  },
    { "kbd",  OVERLAY_COL_CYAN,  poll_kbd  },
    { "sync", OVERLAY_COL_AMBER, poll_sync },
};

/* One cell of the bar's own background. */
static void sb_bar_cell(int x, int row)
{
    ui_pen(OVERLAY_COL_BLUE);
    ui_putch(x, row, ' ', UI_BAR);
}

/* One cell's look. Blink alternates whole styles; OR-ing two would alias
 * to a third (docs/overlay-style.md). */
static void sb_look(const chip_cell_t *c, uint8_t accent, uint32_t frame,
                    uint16_t *cp, uint8_t *style, uint8_t *pen)
{
    *cp = c->cp;
    switch (c->state) {
    case CHIP_ON:
        *style = UI_FOCUS;
        *pen   = c->accent ? c->accent : accent;
        break;
    case CHIP_BUSY:
        if (!c->keep) *cp = spinner_glyph(frame);
        *style = (frame / SB_BLINK_FRAMES) & 1 ? UI_WELL : UI_FOCUS;
        *pen   = OVERLAY_COL_AMBER;
        break;
    case CHIP_ALERT:
        if (!c->keep) *cp = SB_ALERT_ICON;
        *style = UI_FOCUS;
        *pen   = OVERLAY_COL_RED;
        break;
    default:
        *style = UI_WELL;
        *pen   = c->accent ? c->accent : OVERLAY_COL_BLUE;
        break;
    }
}

/* A chip: an opening pad, its cells, a closing pad. The pads borrow the
 * look of the cell they touch, so the run reads as one wash. */
static int sb_chip(int x, int row, const chip_cell_t *cell, int n,
                   uint8_t accent, uint32_t frame)
{
    uint16_t cp;
    uint8_t  style, pen;

    sb_look(&cell[0], accent, frame, &cp, &style, &pen);
    ui_pen(pen);
    ui_putch(x++, row, ' ', style | UI_BOLD);

    for (int i = 0; i < n; i++) {
        sb_look(&cell[i], accent, frame, &cp, &style, &pen);
        ui_pen(pen);
        ui_putch(x++, row, cp, style | UI_BOLD);
    }

    ui_putch(x++, row, ' ', style | UI_BOLD);
    return x;
}

/* Walks the table left to right; returns the first free cell. */
static int sb_cluster(int row)
{
    int x = SB_LEFT_COL;
    for (int i = 0; i < NELEM(CHIPS); i++) {
        chip_cell_t cell[SB_MAX_CELLS] = { { 0 } };
        const int   n = CHIPS[i].poll(cell);
        if (n <= 0 || cell[0].state == CHIP_ABSENT) continue;
        if (x > SB_LEFT_COL)
            for (int g = 0; g < SB_CHIP_GAP; g++) sb_bar_cell(x++, row);
        x = sb_chip(x, row, cell, n, CHIPS[i].accent, app.anim_frame);
    }
    return x;
}

void ui_statusbar(uint64_t now)
{
    const int sr = ui_rows() - 1;
    for (int c = 0; c < ui_cols(); c++)
        sb_bar_cell(c, sr);

    int  clk_x = ui_cols();
    char clk[10];
    if (clock_str(clk + 1, sizeof(clk) - 2)) {
        clk[0] = ' ';
        size_t n = strlen(clk);
        clk[n]     = ' ';
        clk[n + 1] = '\0';
        clk_x = ui_cols() - (int)strlen(clk) - 1;
        ui_puts(clk_x, sr, clk, UI_FOCUS | UI_BOLD);
    }

    /* No keystore-lock chip, by user call 2026-08-27. A locked deck shows
     * the PIN pad, so the state is self-evident. */
    int x = sb_cluster(sr);

    if (app.toast[0] && now < app.toast_until) {
        /* Chips carry the very state a toast announces, so the toast clips
         * into the free gap; taking the whole span is the fallback for a
         * cluster too wide to leave one (docs/status-bar.md). */
        ui_pen(OVERLAY_COL_BLUE);        /* the cluster left its accent set */
        int gap = clk_x - x;
        if (gap < SB_TOAST_MIN_GAP) {
            for (int c = SB_LEFT_COL; c < clk_x; c++)
                sb_bar_cell(c, sr);
            x   = SB_LEFT_COL;
            gap = clk_x - x;
        }
        if (gap > 2)
            ui_printf(x, sr, UI_FOCUS | UI_BOLD, " %.*s ", gap - 2, app.toast);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}
