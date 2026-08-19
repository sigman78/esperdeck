/*
 * app_unlock.c — keystore code entry (ST_UNLOCK).
 *
 * One screen, four phases: UNLOCK (the PIN pad's day job), and the set-code
 * flow OLD → NEW → CONFIRM reached from the menu (create skips OLD). 3x4
 * pad (touch) with BLE-keyboard parity: digits append, Backspace deletes,
 * Enter submits; a passphrase slot is reachable by just typing it.
 * Auto-submits at the known PIN length (keystore_pin_len header hint).
 *
 * Every keystore derivation runs on a worker task — Argon2id takes ~1 s on
 * the S3 and keeps ~1 KiB + hash state on the caller's stack
 * (docs/storage_auth.md), so it must never run inline in a UI tick. The
 * screen animates "DERIVING KEY" meanwhile and swallows input (the
 * derivation is not cancellable). change-code = two derivations (~2 s).
 *
 * Entered from: start_connect() lazily (locked key profile), the saver wake
 * gate and the boot trigger (lock.ini toggles), and the menu's KEYSTORE
 * page (set/change code). Failed-attempt backoff is a later slice.
 */

#include "app_internal.h"
#include "app_menu_defs.h"   /* MS_KEYSTORE (the menu return target) */
#include "app_screens.h"
#include "app_widgets.h"
#include "display_fx.h"
#include "keystore.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NOTE_MS      1200         /* status-row flash duration        */
#define CODE_MIN     4            /* UI floor for a new code's length */
#define PRESS_MS     160          /* pad tile lit after a press       */
#define REVEAL_MS    800          /* newest char shown while defining */
#define IDLE_CANCEL_MS 60000      /* unattended pad cancels to HOME   */

/* Entry phase (app.unlock.mode). */
enum { UM_UNLOCK = 0, UM_OLD, UM_NEW, UM_CONFIRM, UM_REMOVE };

/* Where to land when the screen closes (app.unlock.ret). */
enum { UR_HOME = 0, UR_CONNECT, UR_MENU };

/* Worker op — the keystore call made on the worker task. */
enum { OP_UNLOCK = 0, OP_CREATE, OP_CHANGE, OP_REMOVE };

/* Worker exchange + flow stashes — .bss (internal SRAM), wiped as soon as
 * each value has served its purpose. */
static char          s_try[sizeof(((unlock_state_t *)0)->code)];
static char          s_old[sizeof(s_try)];      /* change flow: current code */
static char          s_staged[sizeof(s_try)];   /* set flow: new, pre-confirm */
static uint8_t       s_op;
static volatile bool s_done;
static volatile int  s_result;

static void unlock_worker(void *arg)
{
    (void)arg;
    esp_err_t r;
    switch (s_op) {
    case OP_CREATE: r = keystore_create(s_try);            break;
    case OP_CHANGE: r = keystore_change_pin(s_old, s_try); break;
    case OP_REMOVE: r = keystore_remove(s_try);            break;
    default:        r = keystore_unlock(s_try);            break;
    }
    memset(s_try, 0, sizeof(s_try));
    memset(s_old, 0, sizeof(s_old));
    s_result = r;
    s_done   = true;
    vTaskDelete(NULL);
}

/* Pad slots: 1 2 3 / 4 5 6 / 7 8 9 / DEL 0 OK. */
static const char *PAD_LBL[12] = { "1", "2", "3", "4", "5", "6",
                                   "7", "8", "9", "DEL", "0", "OK" };

static const char *phase_title(void)
{
    switch (app.unlock.mode) {
    case UM_OLD:     return "CURRENT ACCESS CODE";
    case UM_NEW:     return "NEW ACCESS CODE";
    case UM_CONFIRM: return "CONFIRM NEW CODE";
    case UM_REMOVE:  return "REMOVE KEYSTORE";
    default:         return "ENTER ACCESS CODE";
    }
}

static void render_unlock(void)
{
    ui_colors(UI_FG, UI_BG);
    ui_clear();
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    bool tall = ui_rows() >= 24;
    draw_titlebar(2, phase_title());
    /* Corner tag = WHY this pad is up (two-gates model): the immovable
     * device gate, a key-connect prompt, or a keystore menu flow. */
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - 12, 0,
            app.unlock.gate              ? "// DEVICE  "
          : app.unlock.ret == UR_CONNECT ? "// CONNECT "
                                         : "// KEYSTORE", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    if (tall) draw_rule(3);

    /* Roomy gutters — fat-finger dead zones between the pad tiles; the
     * vertical one only where the row budget allows. */
    tilegrid_t g = { .gx = 3, .gy = tall ? 1 : 0,
                     .ncols = 3, .nrows = 4, .count = 12 };
    g.th = ui_rows() >= 28 ? 4 : 3;
    g.tw = ui_cols() >= 80 ? 10 : 8;
    int dots_row = tall ? 5 : 2;
    g.y0 = dots_row + 2;
    g.x0 = (ui_cols() - (g.tw * 3 + g.gx * 2)) / 2;
    app.grid = g;

    /* Status / dots row: the code being typed, or what the deck is doing
     * with it. A note (wrong code, mismatch) flashes until input resumes. */
    if (app.unlock.deriving) {
        static const char MSG[] = "DERIVING KEY";
        int x = (ui_cols() - (int)sizeof(MSG) + 1) / 2;
        ui_pen(OVERLAY_COL_CYAN);
        ui_putch(x - 2, dots_row, spinner_glyph(app.anim_frame), 0);
        ui_puts(x, dots_row, MSG, 0);
    } else if (app.unlock.note_until) {
        uint8_t blink = ((app.anim_frame / 3) & 1) ? OVERLAY_ATTR_INVERSE : 0;
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts((ui_cols() - (int)strlen(app.unlock.note)) / 2, dots_row,
                app.unlock.note, blink);
    } else if (keystore_backoff_ms() > 0) {
        /* Failed-attempt wait: a live countdown owns the entry row (the
         * ~10 Hz re-render keeps it ticking; submits are refused anyway). */
        char msg[32];
        snprintf(msg, sizeof(msg), "LOCKED \xB7 RETRY IN %u s",
                 (unsigned)((keystore_backoff_ms() + 999) / 1000));
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts((ui_cols() - (int)strlen(msg)) / 2, dots_row, msg,
                OVERLAY_ATTR_BOLD);
    } else {
        /* Entry cells — prominent <●> filled / <○> empty brackets; the
         * newest entry briefly reveals its character while defining a
         * code. Free-length entry appends a blinking caret cell; a row
         * too long for the screen falls back to compact dots. */
        int len = app.unlock.len;
        int n   = app.unlock.expected ? app.unlock.expected : len + 1;
        if (n * 4 - 1 <= ui_cols() - 2) {
            int x0 = (ui_cols() - (n * 4 - 1)) / 2;
            for (int i = 0; i < n; i++) {
                int  x      = x0 + i * 4;
                bool filled = i < len;
                ui_pen(filled ? OVERLAY_COL_GREEN : OVERLAY_COL_DEFAULT);
                ui_putch(x,     dots_row, '<', 0);
                ui_putch(x + 2, dots_row, '>', 0);
                if (filled) {
                    bool show = i == len - 1 && app.unlock.reveal_until;
                    if (show) ui_pen(OVERLAY_COL_WHITE);
                    ui_putch(x + 1, dots_row,
                             show ? (uint16_t)app.unlock.reveal_ch
                                  : UI_LED_ON,
                             OVERLAY_ATTR_BOLD);
                } else if (!app.unlock.expected) {   /* caret cell */
                    if ((app.anim_frame / 4) & 1) {
                        ui_pen(OVERLAY_COL_GREEN);
                        ui_putch(x + 1, dots_row, UI_VBAR, 0);
                    }
                } else {
                    ui_putch(x + 1, dots_row, UI_LED_OFF, 0);
                }
            }
        } else {                       /* long passphrase: compact dots */
            int m = len > 24 ? 24 : len;
            int x0 = (ui_cols() - (m * 2 + 1)) / 2;
            ui_pen(OVERLAY_COL_GREEN);
            for (int i = 0; i < m; i++)
                ui_putch(x0 + i * 2, dots_row, UI_LED_ON, 0);
            if ((app.anim_frame / 4) & 1)
                ui_putch(x0 + m * 2, dots_row, UI_VBAR, 0);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    /* A pressed key is drawn LAST, displaced one cell right+down and lit —
     * the classic button push-in. The full-screen clear each frame erases
     * the displacement when PRESS_MS expires, and drawing it last keeps
     * the shifted tile on top of its lower neighbor on gutterless grids. */
    int lit = app.unlock.press_until ? app.unlock.press : -1;
    for (int i = 0; i < 12; i++) {
        if (i == lit) continue;
        ui_pen(i == 9 ? OVERLAY_COL_AMBER :
               i == 11 ? OVERLAY_COL_GREEN : OVERLAY_COL_CYAN);
        ui_tile(tile_x(&g, i), tile_y(&g, i), g.tw, g.th,
                PAD_LBL[i], "", false);
    }
    if (lit >= 0 && lit < 12) {
        ui_pen(lit == 9 ? OVERLAY_COL_AMBER :
               lit == 11 ? OVERLAY_COL_GREEN : OVERLAY_COL_CYAN);
        ui_tile(tile_x(&g, lit) + 1, tile_y(&g, lit) + 1, g.tw, g.th,
                PAD_LBL[lit], "", true);
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    draw_footer(app.unlock.deriving ? "working..."
              : app.unlock.gate
                  ? "deck locked \xB7 digits \xB7 Enter OK"
              : app.unlock.mode == UM_NEW
                  ? "4+ chars \xB7 Enter OK \xB7 Esc cancel"
              : app.unlock.mode == UM_REMOVE
                  ? "keys revert to plain \xB7 Esc cancel"
                  : "digits \xB7 Enter OK \xB7 Esc cancel");
    ui_no_cursor();
    ui_present();
}

static void flash_note(uint64_t now, const char *msg)
{
    app.unlock.note       = msg;
    app.unlock.note_until = now + NOTE_MS;
    app.next_anim = 0;
}

/* Failed-attempt wait flash — the one dynamic note (static backing). */
static char s_bk_note[24];

static void flash_backoff(uint64_t now)
{
    snprintf(s_bk_note, sizeof(s_bk_note), "WAIT %u s",
             (unsigned)((keystore_backoff_ms() + 999) / 1000));
    flash_note(now, s_bk_note);
}

static void wipe_entry(void)
{
    memset(app.unlock.code, 0, sizeof(app.unlock.code));
    app.unlock.len          = 0;
    app.unlock.reveal_ch    = 0;      /* echoes a code char — wipe with it */
    app.unlock.reveal_until = 0;
}

/* Land wherever this screen was opened from. */
static void unlock_finish(uint64_t now)
{
    wipe_entry();
    memset(s_staged, 0, sizeof(s_staged));
    switch (app.unlock.ret) {
    case UR_CONNECT: connect_resume_active(now); break;
    case UR_MENU:    app.state = ST_MENU; menu_goto(MS_KEYSTORE); break;
    default:         enter_home(now); break;
    }
}

static void unlock_cancel(uint64_t now)
{
    if (app.unlock.gate) return;       /* DEVICE gate: no Esc, no way out */
    memset(s_old, 0, sizeof(s_old));
    if (app.unlock.ret == UR_CONNECT) {
        toast(now, "cancelled");
        app.unlock.ret = UR_HOME;      /* don't arm the connect */
    }
    unlock_finish(now);
}

static void start_worker(uint64_t now, uint8_t op)
{
    memcpy(s_try, app.unlock.code, sizeof(s_try));
    wipe_entry();
    s_op   = op;
    s_done = false;
    app.unlock.deriving = true;
    if (xTaskCreatePinnedToCore(unlock_worker, "ks_unlock", 8192, NULL,
                                5, NULL, 0) != pdPASS) {
        memset(s_try, 0, sizeof(s_try));
        memset(s_old, 0, sizeof(s_old));
        app.unlock.deriving = false;
        toast_for(now, ERR_TOAST_MS, "unlock worker failed");
        enter_home(now);
        return;
    }
    app.next_anim = 0;
    render_unlock();
}

/* Auto-submit length for the CONFIRM phase: mirror an all-digit new code so
 * the confirm feels like the pad it is; passphrases submit on Enter. */
static uint8_t staged_expected(void)
{
    for (const char *p = s_staged; *p; p++)
        if (*p < '0' || *p > '9') return 0;
    size_t n = strlen(s_staged);
    return n <= 24 ? (uint8_t)n : 0;
}

static void enter_phase(uint8_t mode, uint8_t expected)
{
    wipe_entry();
    app.unlock.mode        = mode;
    app.unlock.expected    = expected;
    app.unlock.press_until = 0;       /* don't carry a lit pad key over */
    app.next_anim = 0;
    render_unlock();
}

static void submit(uint64_t now)
{
    if (app.unlock.len == 0 || app.unlock.deriving) return;
    /* Current-code phases refuse to burn a derivation during the wait. */
    if ((app.unlock.mode == UM_UNLOCK || app.unlock.mode == UM_OLD ||
         app.unlock.mode == UM_REMOVE) && keystore_backoff_ms() > 0) {
        wipe_entry();
        flash_backoff(now);
        return;
    }
    switch (app.unlock.mode) {
    case UM_OLD:                       /* stash, then ask for the new code */
        memcpy(s_old, app.unlock.code, sizeof(s_old));
        enter_phase(UM_NEW, 0);
        return;
    case UM_NEW:
        if (app.unlock.len < CODE_MIN) {
            flash_note(now, "AT LEAST 4 CHARACTERS");
            return;                    /* entry kept — extend it */
        }
        memcpy(s_staged, app.unlock.code, sizeof(s_staged));
        enter_phase(UM_CONFIRM, staged_expected());
        return;
    case UM_CONFIRM:
        if (strcmp(app.unlock.code, s_staged) != 0) {
            memset(s_staged, 0, sizeof(s_staged));
            enter_phase(UM_NEW, 0);
            flash_note(now, "CODES DON'T MATCH");
            return;
        }
        memcpy(app.unlock.code, s_staged, sizeof(app.unlock.code));
        memset(s_staged, 0, sizeof(s_staged));
        start_worker(now, app.unlock.creating ? OP_CREATE : OP_CHANGE);
        return;
    case UM_REMOVE:
        start_worker(now, OP_REMOVE);
        return;
    default:
        start_worker(now, OP_UNLOCK);
        return;
    }
}

static void append_char(char c, uint64_t now)
{
    app.unlock.note_until = 0;
    if (app.unlock.len >= (int)sizeof(app.unlock.code) - 1) return;
    app.unlock.code[app.unlock.len++] = c;
    /* Echo the newest character briefly — only while DEFINING a code
     * (typo insurance); unlock/old entry stays fully masked. */
    if (app.unlock.mode == UM_NEW || app.unlock.mode == UM_CONFIRM) {
        app.unlock.reveal_ch    = c;
        app.unlock.reveal_until = now + REVEAL_MS;
    }
    if (app.unlock.expected && app.unlock.len == app.unlock.expected) {
        submit(now);
        return;
    }
    render_unlock();
}

static void erase_char(void)
{
    app.unlock.note_until   = 0;
    app.unlock.reveal_ch    = 0;
    app.unlock.reveal_until = 0;
    if (app.unlock.len > 0)
        app.unlock.code[--app.unlock.len] = '\0';
    render_unlock();
}

void unlock_open(uint64_t now, bool resume_connect)
{
    app.unlock.last_input = now;
    app.unlock.gate     = false;
    app.unlock.ret      = resume_connect ? UR_CONNECT : UR_HOME;
    app.unlock.creating = false;
    app.unlock.deriving = false;
    app.unlock.note_until = 0;
    app.state = ST_UNLOCK;
    enter_phase(UM_UNLOCK, keystore_pin_len());
}

void unlock_open_gate(uint64_t now)
{
    unlock_open(now, false);
    app.unlock.gate = true;            /* non-skippable, rain-capable */
}

void unlock_open_setpin(uint64_t now)
{
    app.unlock.last_input = now;
    app.unlock.gate     = false;
    app.unlock.ret      = UR_MENU;
    app.unlock.creating = keystore_state() == KEYSTORE_ABSENT;
    app.unlock.deriving = false;
    app.unlock.note_until = 0;
    app.state = ST_UNLOCK;
    if (app.unlock.creating) enter_phase(UM_NEW, 0);
    else                     enter_phase(UM_OLD, keystore_pin_len());
}

void unlock_open_remove(uint64_t now)
{
    app.unlock.last_input = now;
    app.unlock.gate     = false;
    app.unlock.ret      = UR_MENU;
    app.unlock.creating = false;
    app.unlock.deriving = false;
    app.unlock.note_until = 0;
    app.state = ST_UNLOCK;
    enter_phase(UM_REMOVE, keystore_pin_len());
}

/* The worker finished — route the result by op. */
static void worker_result(uint64_t now, esp_err_t r)
{
    if (r == KEYSTORE_ERR_BACKOFF) {   /* wait armed mid-flow; nothing ran */
        if (s_op == OP_CHANGE) enter_phase(UM_OLD, keystore_pin_len());
        flash_backoff(now);
        return;
    }
    if (s_op == OP_UNLOCK) {
        if (r == ESP_OK) {
            toast(now, app.unlock.gate ? "deck unlocked"
                                       : "keystore unlocked");
            unlock_finish(now);
        } else if (r == ESP_FAIL) {                 /* wrong code */
            display_bell();
            display_fx_static_brief();
            flash_note(now, "ACCESS DENIED");
        } else {                                    /* corrupt / OOM / gone */
            toast_for(now, ERR_TOAST_MS, "keystore error (%d)", (int)r);
            enter_home(now);
        }
        return;
    }
    if (r == ESP_OK) {
        toast(now, s_op == OP_CREATE ? "keystore created"
                 : s_op == OP_REMOVE ? "keystore removed \xB7 keys in plain"
                                     : "code changed");
        unlock_finish(now);
        return;
    }
    if ((s_op == OP_CHANGE || s_op == OP_REMOVE) && r == ESP_FAIL) {
        display_bell();                             /* wrong current code */
        display_fx_static_brief();
        enter_phase(s_op == OP_REMOVE ? UM_REMOVE : UM_OLD,
                    keystore_pin_len());
        flash_note(now, "WRONG CURRENT CODE");
        return;
    }
    toast_for(now, ERR_TOAST_MS, "keystore error (%d)", (int)r);
    unlock_finish(now);
}

void unlock_tick(uint64_t now)
{
    if (app.unlock.deriving && s_done) {
        app.unlock.deriving   = false;
        app.unlock.last_input = now;   /* fresh idle window after a bounce */
        wipe_entry();
        worker_result(now, (esp_err_t)s_result);
        if (app.state != ST_UNLOCK) return;
    }
    if (app.unlock.note_until && now >= app.unlock.note_until)
        app.unlock.note_until = 0;
    if (app.unlock.press_until && now >= app.unlock.press_until)
        app.unlock.press_until = 0;
    if (app.unlock.reveal_until && now >= app.unlock.reveal_until) {
        app.unlock.reveal_until = 0;
        app.unlock.reveal_ch    = 0;
    }

    /* An unattended pad must not sit lit forever, and the two pad kinds
     * resolve that differently. The DEVICE gate cannot cancel (that would
     * be a bypass): the rain falls over it and wake lands right back on
     * the pad. A cancellable pad — connect fallback, abandoned set-code
     * flow — gives up after a minute and lands on HOME. */
    if (!app.unlock.deriving) {
        if (app.unlock.gate) {
            if (saver_tick_gate(now)) return;   /* rain owns the screen */
        } else if (now - app.unlock.last_input >= IDLE_CANCEL_MS) {
            if (app.unlock.ret == UR_MENU) app.unlock.ret = UR_HOME;
            unlock_cancel(now);
            return;
        }
    }

    if (now >= app.next_anim) {                     /* spinner/caret/blink */
        app.next_anim = now + ANIM_PERIOD_MS;
        render_unlock();
    }
}

/* Light the pad tile under a press for PRESS_MS (touch, or the matching
 * key) — set BEFORE dispatch so the action's own render already shows it. */
static void pad_flash(int slot, uint64_t now)
{
    app.unlock.press       = (int8_t)slot;
    app.unlock.press_until = now + PRESS_MS;
}

void unlock_input(const cyberdeck_input_t *ev, ui_key_t k, char ch, uint64_t now)
{
    app.unlock.last_input = now;           /* any touch/key holds the pad */
    if (app.unlock.deriving) return;       /* KDF running: not cancellable */

    if (ev->type == CYBERDECK_INPUT_TAP) {
        int slot = tile_hit(&app.grid, ev->x, ev->y);
        if (slot < 0) return;
        pad_flash(slot, now);
        if (slot == 9)       erase_char();
        else if (slot == 11) submit(now);
        else                 append_char(slot == 10 ? '0' : (char)('1' + slot),
                                         now);
        return;
    }
    if (ev->type != CYBERDECK_INPUT_KEY) return;

    if (k == K_CHAR && ch >= '0' && ch <= '9')
        pad_flash(ch == '0' ? 10 : ch - '1', now);
    else if (k == K_BACKSPACE) pad_flash(9, now);
    else if (k == K_ENTER)     pad_flash(11, now);

    if (k == K_ESC)            unlock_cancel(now);
    else if (k == K_ENTER)     submit(now);
    else if (k == K_BACKSPACE) erase_char();
    else if (k == K_CHAR)      append_char(ch, now);
}
