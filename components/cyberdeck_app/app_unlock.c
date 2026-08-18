/*
 * app_unlock.c — keystore unlock screen (ST_UNLOCK).
 *
 * 3x4 PIN pad (touch) with BLE-keyboard parity: digits append, Backspace
 * deletes, Enter submits; a passphrase slot is reachable by just typing it.
 * Auto-submits at the configured PIN length (keystore_pin_len header hint).
 *
 * The KDF runs on a worker task — Argon2id takes ~1 s on the S3 and keeps
 * ~1 KiB + hash state on the caller's stack (docs/storage_auth.md), so it
 * must never run inline in a UI tick. The screen animates "DERIVING KEY"
 * meanwhile and swallows input (the derivation is not cancellable).
 *
 * Entered lazily from start_connect() when a key profile needs the locked
 * store; boot/wake triggers and failed-attempt backoff are later slices.
 */

#include "app_internal.h"
#include "app_screens.h"
#include "app_widgets.h"
#include "display_fx.h"
#include "keystore.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DENIED_MS  1200           /* "ACCESS DENIED" flash duration */

/* Worker exchange — .bss (internal SRAM); the copy is wiped by the worker
 * as soon as keystore_unlock returns. */
static char          s_try[sizeof(((unlock_state_t *)0)->code)];
static volatile bool s_done;
static volatile int  s_result;

static void unlock_worker(void *arg)
{
    (void)arg;
    s_result = keystore_unlock(s_try);
    memset(s_try, 0, sizeof(s_try));
    s_done = true;
    vTaskDelete(NULL);
}

/* Pad slots: 1 2 3 / 4 5 6 / 7 8 9 / DEL 0 OK. */
static const char *PAD_LBL[12] = { "1", "2", "3", "4", "5", "6",
                                   "7", "8", "9", "DEL", "0", "OK" };

static void render_unlock(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    bool tall = ui_rows() >= 24;
    draw_titlebar(2, "ENTER ACCESS CODE");
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - 12, 0, "// KEYSTORE", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    if (tall) draw_rule(3);

    tilegrid_t g = { .gx = 2, .gy = 0, .ncols = 3, .nrows = 4, .count = 12 };
    g.th = ui_rows() >= 28 ? 4 : 3;
    g.tw = ui_cols() >= 80 ? 10 : 8;
    int dots_row = tall ? 5 : 2;
    g.y0 = dots_row + 2;
    g.x0 = (ui_cols() - (g.tw * 3 + g.gx * 2)) / 2;
    app.grid = g;

    /* Status / dots row: the code being typed, or what the deck is doing
     * with it. Wrong code flashes ACCESS DENIED (input clears it). */
    if (app.unlock.deriving) {
        static const char MSG[] = "DERIVING KEY";
        int x = (ui_cols() - (int)sizeof(MSG) + 1) / 2;
        ui_pen(OVERLAY_COL_CYAN);
        ui_putch(x - 2, dots_row, spinner_glyph(app.anim_frame), 0);
        ui_puts(x, dots_row, MSG, 0);
    } else if (app.unlock.denied_until) {
        static const char MSG[] = "ACCESS DENIED";
        uint8_t blink = ((app.anim_frame / 3) & 1) ? OVERLAY_ATTR_INVERSE : 0;
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts((ui_cols() - (int)sizeof(MSG) + 1) / 2, dots_row, MSG, blink);
    } else {
        /* Fixed slots when the length is known (filled ● / hollow ○);
         * free-length entry grows dot by dot behind a blinking caret. */
        int n = app.unlock.expected ? app.unlock.expected : app.unlock.len;
        if (n > 24) n = 24;
        int x0 = (ui_cols() - (n * 2 + (app.unlock.expected ? -1 : 1))) / 2;
        for (int i = 0; i < n; i++) {
            bool filled = i < app.unlock.len;
            ui_pen(filled ? OVERLAY_COL_GREEN : OVERLAY_COL_DEFAULT);
            ui_putch(x0 + i * 2, dots_row, filled ? UI_LED_ON : UI_LED_OFF, 0);
        }
        if (!app.unlock.expected && ((app.anim_frame / 4) & 1)) {
            ui_pen(OVERLAY_COL_GREEN);
            ui_putch(x0 + n * 2, dots_row, UI_VBAR, 0);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    for (int i = 0; i < 12; i++) {
        ui_pen(i == 9 ? OVERLAY_COL_AMBER :
               i == 11 ? OVERLAY_COL_GREEN : OVERLAY_COL_CYAN);
        ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th,
                PAD_LBL[i], "", false);
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_footer(app.unlock.deriving ? "unlocking..."
                                    : "digits \xB7 Enter OK \xB7 Esc cancel");
    ui_no_cursor();
    ui_present();
}

static void wipe_entry(void)
{
    memset(app.unlock.code, 0, sizeof(app.unlock.code));
    app.unlock.len = 0;
}

static void unlock_cancel(uint64_t now)
{
    wipe_entry();
    if (app.unlock.resume)
        toast(now, "cancelled");
    enter_home(now);
}

static void submit(uint64_t now)
{
    if (app.unlock.len == 0 || app.unlock.deriving) return;
    memcpy(s_try, app.unlock.code, sizeof(s_try));
    wipe_entry();
    s_done = false;
    app.unlock.deriving = true;
    if (xTaskCreatePinnedToCore(unlock_worker, "ks_unlock", 8192, NULL,
                                5, NULL, 0) != pdPASS) {
        memset(s_try, 0, sizeof(s_try));
        app.unlock.deriving = false;
        toast_for(now, ERR_TOAST_MS, "unlock worker failed");
        enter_home(now);
        return;
    }
    app.next_anim = 0;
    render_unlock();
}

static void append_char(char c, uint64_t now)
{
    app.unlock.denied_until = 0;
    if (app.unlock.len >= (int)sizeof(app.unlock.code) - 1) return;
    app.unlock.code[app.unlock.len++] = c;
    if (app.unlock.expected && app.unlock.len == app.unlock.expected) {
        submit(now);
        return;
    }
    render_unlock();
}

static void erase_char(void)
{
    app.unlock.denied_until = 0;
    if (app.unlock.len > 0)
        app.unlock.code[--app.unlock.len] = '\0';
    render_unlock();
}

void unlock_open(uint64_t now, bool resume_connect)
{
    (void)now;
    wipe_entry();
    app.unlock.deriving     = false;
    app.unlock.resume       = resume_connect;
    app.unlock.denied_until = 0;
    app.unlock.expected     = keystore_pin_len();
    app.next_anim = 0;
    app.state     = ST_UNLOCK;
    render_unlock();
}

void unlock_tick(uint64_t now)
{
    if (app.unlock.deriving && s_done) {
        app.unlock.deriving = false;
        esp_err_t r = (esp_err_t)s_result;
        if (r == ESP_OK) {
            toast(now, "keystore unlocked");
            if (app.unlock.resume) connect_resume_active(now);
            else                   enter_home(now);
            return;
        }
        if (r == ESP_FAIL) {                        /* wrong code */
            display_bell();
            display_fx_static_brief();
            app.unlock.denied_until = now + DENIED_MS;
        } else {                                    /* corrupt / OOM / gone */
            toast_for(now, ERR_TOAST_MS, "keystore error (%d)", (int)r);
            enter_home(now);
            return;
        }
    }
    if (app.unlock.denied_until && now >= app.unlock.denied_until)
        app.unlock.denied_until = 0;

    if (now >= app.next_anim) {                     /* spinner/caret/blink */
        app.next_anim = now + ANIM_PERIOD_MS;
        render_unlock();
    }
}

void unlock_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    if (app.unlock.deriving) return;       /* ~1 s KDF: not cancellable */

    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot < 0) return;
        if (slot == 9)       erase_char();
        else if (slot == 11) submit(now);
        else                 append_char(slot == 10 ? '0' : (char)('1' + slot),
                                         now);
        return;
    }
    if (ev->type != CYBERDECK_INPUT_KEY) return;

    if (k == K_ESC)            unlock_cancel(now);
    else if (k == K_ENTER)     submit(now);
    else if (k == K_BACKSPACE) erase_char();
    else if (k == K_CHAR)      append_char(ch, now);
}
