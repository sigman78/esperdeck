/*
 * app_statusbar.c — the StatusBar chip engine (docs/status-bar.md).
 *
 * A chip is 1-2 adjacent segments whose state (ABSENT/OFF/ON/BUSY/ALERT)
 * picks the style+pen pair; the walk polls a static table every frame.
 * The chip types stay file-internal until the plugin seam (step 5).
 */

#include "app_widgets.h"
#include "wifi_manager.h"

#include <string.h>

#define SB_LEFT_COL       1    /* first cell of the left cluster        */
#define SB_MAX_SEGS       2    /* icon segment + one fused sub-state    */
#define SB_BLINK_FRAMES   8    /* BUSY blink half-period, ~1 Hz at 10 fps */
#define SB_RSSI_FRAMES    10   /* RSSI snapshot cadence, ~1 s at 10 fps  */
#define SB_TOAST_MIN_GAP  16   /* under this the toast takes the span    */

/* HOME's link-quality buckets, dBm. */
#define SB_RSSI_GOOD   (-55)
#define SB_RSSI_FAIR   (-67)
#define SB_RSSI_WEAK   (-78)

/* Lower-eighth block ramp: the NET stair is one cell, height = quality. */
#define SB_STAIR_1     0x2582u
#define SB_STAIR_2     0x2584u
#define SB_STAIR_3     0x2586u
#define SB_STAIR_4     0x2588u
#define SB_ALERT_ICON  0x2718u   /* heavy ballot X */

typedef enum {
    CHIP_ABSENT = 0, CHIP_OFF, CHIP_ON, CHIP_BUSY, CHIP_ALERT
} chip_state_t;

typedef struct {
    chip_state_t state;
    uint16_t     icon;     /* codepoint for the single icon cell        */
    uint8_t      accent;   /* ON accent override; 0 = descriptor's own  */
} chip_seg_t;

typedef struct {
    const char *id;        /* stable key for the plugin seam            */
    uint8_t     accent;    /* default ON accent                         */
    int       (*poll)(chip_seg_t seg[SB_MAX_SEGS]);   /* fill, return count */
} chip_desc_t;

static int poll_net(chip_seg_t seg[SB_MAX_SEGS])
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

    seg[0].icon = rssi > SB_RSSI_GOOD ? SB_STAIR_4
                : rssi > SB_RSSI_FAIR ? SB_STAIR_3
                : rssi > SB_RSSI_WEAK ? SB_STAIR_2 : SB_STAIR_1;
    switch (wifi_manager_get_state()) {
    case WIFI_MGR_CONNECTED:  seg[0].state = CHIP_ON;    break;
    case WIFI_MGR_CONNECTING:
    case WIFI_MGR_LOST:       seg[0].state = CHIP_BUSY;  break;
    case WIFI_MGR_FAILED:     seg[0].state = CHIP_ALERT; break;
    default:
        /* get_rssi reads 0 while down, which would round up to a full
         * stair; the shape says "signal", the well says "down". */
        seg[0].state = CHIP_OFF;
        seg[0].icon  = SB_STAIR_1;
        break;
    }
    return 1;
}

static int poll_kbd(chip_seg_t seg[SB_MAX_SEGS])
{
    if (!app.ble || !app.ble->get_state) return 0;

    seg[0].icon = 'K';   /* letter stands in until the sprite set lands */
    const cyberdeck_ble_state_t st = app.ble->get_state();
    switch (st) {
    case CYBERDECK_BLE_CONNECTED:    seg[0].state = CHIP_ON;   break;
    case CYBERDECK_BLE_CONNECTING:
    case CYBERDECK_BLE_PAIRING_SCAN:
    case CYBERDECK_BLE_RECONNECT:    seg[0].state = CHIP_BUSY; break;
    default:                         seg[0].state = CHIP_OFF;  break;
    }

    /* Num lock never renders — the keymap ignores it, so an indicator
     * would have no referent (get_locks still reports the bit). */
    if (st == CYBERDECK_BLE_CONNECTED && app.ble->get_locks &&
        (app.ble->get_locks() & CYBERDECK_KBD_LOCK_CAPS)) {
        seg[1].state  = CHIP_ON;
        seg[1].icon   = 'C';
        seg[1].accent = OVERLAY_COL_AMBER;
        return 2;
    }
    return 1;
}

static const chip_desc_t CHIPS[] = {
    { "net", OVERLAY_COL_GREEN, poll_net },
    { "kbd", OVERLAY_COL_CYAN,  poll_kbd },
};

/* One segment: a padding cell, the icon, and — for the chip's primary
 * segment — a closing padding cell. Blink alternates whole styles; OR-ing
 * two would alias to a third (docs/overlay-style.md). */
static int sb_seg(int x, int row, const chip_seg_t *s, uint8_t accent,
                  bool primary)
{
    uint16_t icon = s->icon;
    uint8_t  style, pen;
    switch (s->state) {
    case CHIP_ON:
        style = UI_FOCUS;
        pen   = s->accent ? s->accent : accent;
        break;
    case CHIP_BUSY:
        icon  = spinner_glyph(app.anim_frame);
        style = (app.anim_frame / SB_BLINK_FRAMES) & 1 ? UI_WELL : UI_FOCUS;
        pen   = OVERLAY_COL_AMBER;
        break;
    case CHIP_ALERT:
        icon  = SB_ALERT_ICON;
        style = UI_FOCUS;
        pen   = OVERLAY_COL_RED;
        break;
    default:
        style = UI_WELL;
        pen   = OVERLAY_COL_BLUE;
        break;
    }
    ui_pen(pen);
    ui_putch(x++, row, ' ', style | UI_BOLD);
    ui_putch(x++, row, icon, style | UI_BOLD);
    if (primary) ui_putch(x++, row, ' ', style | UI_BOLD);
    return x;
}

/* Walks the table left to right, no gaps; returns the first free cell. */
static int sb_cluster(int row)
{
    int x = SB_LEFT_COL;
    for (int i = 0; i < NELEM(CHIPS); i++) {
        chip_seg_t seg[SB_MAX_SEGS] = { { 0 } };
        const int n = CHIPS[i].poll(seg);
        for (int s = 0; s < n && s < SB_MAX_SEGS; s++)
            if (seg[s].state != CHIP_ABSENT)
                x = sb_seg(x, row, &seg[s], CHIPS[i].accent, s == 0);
    }
    return x;
}

void ui_statusbar(uint64_t now)
{
    const int sr = ui_rows() - 1;
    ui_pen(OVERLAY_COL_BLUE);
    for (int c = 0; c < ui_cols(); c++)
        ui_putch(c, sr, ' ', UI_BAR);

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
                ui_putch(c, sr, ' ', UI_BAR);
            x   = SB_LEFT_COL;
            gap = clk_x - x;
        }
        if (gap > 2)
            ui_printf(x, sr, UI_FOCUS | UI_BOLD, " %.*s ", gap - 2, app.toast);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}
