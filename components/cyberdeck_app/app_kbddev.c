/*
 * app_kbddev.c - the paired-keyboard page (SCR_KBDDEV): what a tap on a
 * Setup > Keyboard row opens. Facts of one bond and Forget set apart at
 * the bottom. Forget asks first (app_ask.c). Also the bond helpers the
 * section shares.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "storage.h"

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

_Static_assert(KBD_BOND_MAX >= STORAGE_BLE_MAX, "bond list too small");

enum { ID_FORGET = 1 };

static const char *TAG = "app_kbddev";

int kbd_bonds(cyberdeck_ble_bond_t *out, int max)
{
    if (!app.ble || !app.ble->bonds) return 0;
    const int n = app.ble->bonds(out, max);
    return n < 0 ? 0 : n > max ? max : n;
}

bool kbd_connected_addr(uint8_t addr[6])
{
    return app.ble && app.ble->connected_addr && app.ble->connected_addr(addr);
}

bool kbd_is_connected(const cyberdeck_ble_bond_t *b)
{
    uint8_t a[6];
    return kbd_connected_addr(a) && memcmp(a, b->addr, 6) == 0;
}

void kbd_addr_str(const uint8_t a[6], char *buf, size_t sz)
{
    /* Stored little-endian (NimBLE); shown most significant byte first. */
    snprintf(buf, sz, "%02X:%02X:%02X:%02X:%02X:%02X",
             a[5], a[4], a[3], a[2], a[1], a[0]);
}

void kbd_label(const cyberdeck_ble_bond_t *b, char *buf, size_t sz)
{
    if (b->name[0]) snprintf(buf, sz, "%s", b->name);
    else kbd_addr_str(b->addr, buf, sz);
}

/* The bond as the page last read it. */
static struct {
    cyberdeck_ble_bond_t b;
    bool                 have;
    ui_id_t              saved_focus;
} s_kbd;

static bool can_forget(void)
{
    return app.ble && app.ble->forget_device;
}

static void render_kbddev(uint64_t now)
{
    (void)now;
    const int faults = ui_faults();
    ui_slate_t pg = ui_slate_page();
    if (!s_kbd.have) return;
    char label[40], addr[18];
    kbd_label(&s_kbd.b, label, sizeof label);
    kbd_addr_str(s_kbd.b.addr, addr, sizeof addr);
    const bool conn = kbd_is_connected(&s_kbd.b);

    const ui_info_row_t rows[3] = {
        { .label = "Name", .value = s_kbd.b.name[0] ? s_kbd.b.name : "-" },
        { .label = "Address", .value = addr },
        conn ? (ui_info_row_t){ .label = "State", .value = "connected",
                                .tail = UI_S_CHECK,
                                .tail_pen = OVERLAY_COL_GREEN }
             : (ui_info_row_t){ .label = "State", .value = "paired" },
    };

    ui_focus_default(ID_FORGET);
    ui_title(pg.header, "Keyboard", label, NULL, true);
    ui_info(ui_cut_top(&pg.body, 5), rows, 3, conn ? OVERLAY_COL_GREEN : 0);
    ui_push(ui_rect(pg.body.x, pg.body.y + pg.body.h - 3,
                    ui_push_w("Forget"), 3),
            ID_FORGET, "Forget", UI_TONE_TINT, OVERLAY_COL_RED,
            can_forget() ? 0 : UI_PART_OFF);

    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

static void kbddev_enter(intptr_t arg, uint64_t now)
{
    (void)now;
    cyberdeck_ble_bond_t b[KBD_BOND_MAX];
    const int n = kbd_bonds(b, KBD_BOND_MAX);
    s_kbd.have = arg >= 0 && arg < n;
    if (s_kbd.have) s_kbd.b = b[arg];
    s_kbd.saved_focus = 0;
}

/* Back from the question: keep the focus where it was. */
static void kbddev_resume(intptr_t arg, uint64_t now)
{
    (void)arg; (void)now;
    if (s_kbd.saved_focus) ui_focus_set(s_kbd.saved_focus);
}

static void kbddev_exit(uint64_t now)
{
    (void)now;
    s_kbd.saved_focus = ui_focus_get();
}

/* The page follows the link: the state row flips when it connects. */
static void kbddev_tick(uint64_t now)
{
    if (!s_kbd.have) {
        nav_pop(now);
        return;
    }
    if (now >= app.next_anim) {
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

static void kbd_forget_yes(intptr_t arg, uint64_t now)
{
    (void)arg;
    if (can_forget()) app.ble->forget_device(s_kbd.b.addr, s_kbd.b.addr_type);
    toast(now, "keyboard forgotten");
    nav_pop(now);
}

static void kbd_ask_forget(uint64_t now)
{
    char label[40], title[64];
    kbd_label(&s_kbd.b, label, sizeof label);
    snprintf(title, sizeof title, "Forget %s?", label);
    ask_open(&(ask_t){
        .title = title, .line1 = "Pair it again to use it.",
        .line2 = kbd_is_connected(&s_kbd.b) ? "It disconnects now." : NULL,
        .verb = "Forget", .accent = OVERLAY_COL_RED,
        .yes = kbd_forget_yes,
    }, now);
}

static void kbddev_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                         uint64_t now)
{
    (void)ch;
    if (k == K_ESC) {
        nav_pop(now);
        return;
    }
    const ui_act_t a = app_hot_input(ev, k);
    if (a.kind == UI_ACT_NONE) return;
    nav_invalidate();
    if (a.kind != UI_ACT_PRESS || !s_kbd.have) return;
    if (a.id == UI_ID_BACK) nav_pop(now);
    else if (a.id == ID_FORGET && can_forget()) kbd_ask_forget(now);
}

void enter_kbddev(uint64_t now, int idx)
{
    nav_push(SCR_KBDDEV, idx, now);
}

const nav_screen_t kbddev_screen = {
    .name = "kbddev", .enter = kbddev_enter, .resume = kbddev_resume,
    .exit = kbddev_exit, .tick = kbddev_tick, .input = kbddev_input,
    .render = render_kbddev, .chrome = NAV_CHROME_FULL,
};
