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
 * Entered from: the boot and saver-wake gates (creating a store IS opting
 * into the lock — see docs/storage_auth.md "two-gates model"), start_connect()
 * lazily (locked key profile), and the menu's KEYSTORE page (set/change code).
 */

#include "app_internal.h"
#include "app_menu_defs.h"   /* MS_KEYSTORE (the menu return target) */
#include "app_screens.h"
#include "app_widgets.h"
#include "display_fx.h"
#include "keystore.h"
#include "wifi_manager.h"    /* post-unlock PSK re-kick */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static struct {
    char     code[65];              /* typed code, KEYSTORE_PIN_MAX + 1;
                                     * .bss = internal SRAM; wiped on
                                     * submit/leave                        */
    int      len;
    uint8_t  expected;              /* auto-submit length (0 = Enter only) */
    uint8_t  mode;                  /* entry phase (um_mode)               */
    bool     creating;              /* set-code flow on an ABSENT store    */
    bool     deriving;              /* KDF worker running; input swallowed */
    const char *note;               /* status-row flash (static string)    */
    uint64_t note_until;
    int8_t   press;                 /* pad slot lit by a press...          */
    uint64_t press_until;           /* ...until then (0 = none lit)        */
    uint64_t last_input;            /* idle-cancel clock (IDLE_CANCEL_MS)  */
    char     reveal_ch;             /* newest typed char, echoed briefly...*/
    uint64_t reveal_until;          /* ...while defining a code (0 = off)  */
    /* Two-gates model: a keystore on the deck means the deck is LOCKED.
     * gate = this pad is the DEVICE gate (boot/wake): non-skippable, no
     * idle-cancel, the saver rains over it. */
    bool     gate;
} s_unlock;

#define NOTE_MS      1200         /* status-row flash duration        */
#define CODE_MIN     4            /* UI floor for a new code's length */
#define PRESS_MS     160          /* pad tile lit after a press       */
#define REVEAL_MS    800          /* newest char shown while defining */
#define IDLE_CANCEL_MS 60000      /* unattended pad cancels to HOME   */

/* Entry phase (s_unlock.mode). */
enum { UM_UNLOCK = 0, UM_OLD, UM_NEW, UM_CONFIRM, UM_REMOVE };

/* Intent arg (nav): why this pad is up. Where to land when it closes is
 * the nav stack's business — except the connect continuation. */
enum { UA_PROMPT = 0, UA_RESUME, UA_GATE, UA_SETPIN, UA_REMOVE };
static uint8_t s_flavor;

/* Worker op — the keystore call made on the worker task. */
enum { OP_UNLOCK = 0, OP_CREATE, OP_CHANGE, OP_REMOVE };

/* Worker exchange + flow stashes — .bss (internal SRAM), wiped as soon as
 * each value has served its purpose. */
static char          s_try[sizeof(s_unlock.code)];
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
    switch (s_unlock.mode) {
    case UM_OLD:     return "CURRENT ACCESS CODE";
    case UM_NEW:     return "NEW ACCESS CODE (4+)";
    case UM_CONFIRM: return "CONFIRM NEW CODE";
    case UM_REMOVE:  return "REMOVE KEYSTORE";
    default:         return "ENTER ACCESS CODE";
    }
}

static void render_unlock(uint64_t now)
{
    (void)now;
    ui_fill(0, 0, ui_cols(), ui_rows(), 0);

    bool tall = ui_rows() >= 24;
    draw_titlebar(2, phase_title());
    /* Corner tag = WHY this pad is up (two-gates model): the immovable
     * device gate, a key-connect prompt, or a keystore menu flow. */
    ui_pen(OVERLAY_COL_BLUE);
    ui_puts(ui_cols() - 12, 0,
            s_unlock.gate          ? "// DEVICE  "
          : s_flavor == UA_RESUME    ? "// CONNECT "
                                     : "// KEYSTORE", 0);
    ui_pen(OVERLAY_COL_DEFAULT);
    if (tall) draw_rule(3);

    /* Roomy gutters — fat-finger dead zones between the pad tiles; the
     * vertical one only where the row budget allows. */
    tilegrid_t g = { .gx = 3, .gy = tall ? 1 : 0,
                     .ncols = 3, .nrows = 4, .count = 12 };
    g.th = ui_rows() >= 28 ? 4 : 3;
    g.tw = ui_cols() >= 80 ? 10 : 8;
    int entry_row = tall ? 5 : 2;
    g.y0 = entry_row + 2;
    g.x0 = (ui_cols() - (g.tw * 3 + g.gx * 2)) / 2;
    app.grid = g;

    /* Status / entry row: the code being typed, or what the deck is doing
     * with it. A note (wrong code, mismatch) flashes until input resumes. */
    if (s_unlock.deriving) {
        static const char MSG[] = "DERIVING KEY";
        int x = (ui_cols() - (int)sizeof(MSG) + 1) / 2;
        ui_pen(OVERLAY_COL_CYAN);
        ui_putch(x - 2, entry_row, spinner_glyph(app.anim_frame), 0);
        ui_puts(x, entry_row, MSG, 0);
    } else if (s_unlock.note_until) {
        uint8_t blink = ((app.anim_frame / 3) & 1) ? OVERLAY_ATTR_INVERSE : 0;
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts((ui_cols() - (int)strlen(s_unlock.note)) / 2, entry_row,
                s_unlock.note, blink);
    } else if (keystore_backoff_ms() > 0) {
        /* Failed-attempt wait: a live countdown owns the entry row (the
         * ~10 Hz re-render keeps it ticking; submits are refused anyway). */
        char msg[32];
        snprintf(msg, sizeof(msg), "LOCKED \xB7 RETRY IN %u s",
                 (unsigned)((keystore_backoff_ms() + 999) / 1000));
        ui_pen(OVERLAY_COL_AMBER);
        ui_puts((ui_cols() - (int)strlen(msg)) / 2, entry_row, msg,
                OVERLAY_ATTR_BOLD);
    } else {
        /* Entry cells — [X] taken / [_] still to come, one bracketed slot
         * per character with a gap between them: at a glance the row reads
         * as "three of four", which the old ●/○ pair did not. The newest
         * entry briefly shows its character while defining a code. A
         * free-length row appends a blinking caret cell; a row too long
         * for the screen falls back to compact marks. */
        int len = s_unlock.len;
        int n   = s_unlock.expected ? s_unlock.expected : len + 1;
        if (n * 4 - 1 <= ui_cols() - 2) {
            int x0 = (ui_cols() - (n * 4 - 1)) / 2;
            for (int i = 0; i < n; i++) {
                int     x      = x0 + i * 4;
                bool    filled = i < len;
                /* Empty slots recede (dim), taken ones step forward (bold
                 * green) — on this all-green theme the glyph alone would
                 * not separate them. */
                uint8_t a      = filled ? OVERLAY_ATTR_BOLD : OVERLAY_ATTR_DIM;
                ui_pen(filled ? OVERLAY_COL_GREEN : OVERLAY_COL_DEFAULT);
                ui_putch(x,     entry_row, '[', a);
                ui_putch(x + 2, entry_row, ']', a);
                if (filled) {
                    bool show = i == len - 1 && s_unlock.reveal_until;
                    if (show) ui_pen(OVERLAY_COL_WHITE);
                    ui_putch(x + 1, entry_row,
                             show ? (uint16_t)s_unlock.reveal_ch : 'X', a);
                } else if (!s_unlock.expected) {   /* caret cell */
                    ui_pen(OVERLAY_COL_GREEN);
                    ui_putch(x + 1, entry_row,
                             ((app.anim_frame / 4) & 1) ? UI_VBAR : '_',
                             OVERLAY_ATTR_BOLD);
                } else {
                    ui_putch(x + 1, entry_row, '_', a);
                }
            }
        } else {                       /* long passphrase: compact marks */
            int m = len > 24 ? 24 : len;
            int x0 = (ui_cols() - (m * 2 + 1)) / 2;
            ui_pen(OVERLAY_COL_GREEN);
            for (int i = 0; i < m; i++)
                ui_putch(x0 + i * 2, entry_row, 'X', OVERLAY_ATTR_BOLD);
            if ((app.anim_frame / 4) & 1)
                ui_putch(x0 + m * 2, entry_row, UI_VBAR, 0);
        }
    }
    ui_pen(OVERLAY_COL_DEFAULT);

    /* A pressed tile is drawn LAST, displaced one cell right+down and lit —
     * the classic button push-in. The full-screen clear each frame erases
     * the displacement when PRESS_MS expires, and drawing it last keeps
     * the shifted tile on top of its lower neighbor on gutterless grids.
     * Only touch lights a tile (see unlock_input): mirroring keystrokes
     * onto the pad would shoulder-surf the code onto a screen the typist
     * isn't even looking at. */
    int lit = s_unlock.press_until ? s_unlock.press : -1;
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

    /* No mode line: the title, the corner tag, the pad and the live
     * entry row already say everything (redundancy call, 2026-08-27). */
}

static void flash_note(uint64_t now, const char *msg)
{
    s_unlock.note       = msg;
    s_unlock.note_until = now + NOTE_MS;
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
    memset(s_unlock.code, 0, sizeof(s_unlock.code));
    s_unlock.len          = 0;
    s_unlock.reveal_ch    = 0;      /* echoes a code char — wipe with it */
    s_unlock.reveal_until = 0;
}

/* Land wherever this screen was opened from. */
static void unlock_finish(uint64_t now)
{
    wipe_entry();
    memset(s_staged, 0, sizeof(s_staged));
    switch (s_flavor) {
    case UA_RESUME:                       /* re-arm the gated connect */
        connect_resume_active(now);
        break;
    case UA_SETPIN:
    case UA_REMOVE:                       /* back to the KEYSTORE page */
        nav_pop(now);
        break;
    default:
        enter_home(now);
        break;
    }
}

static void unlock_cancel(uint64_t now)
{
    if (s_unlock.gate) return;       /* DEVICE gate: no Esc, no way out */
    memset(s_old, 0, sizeof(s_old));
    if (s_flavor == UA_RESUME) {
        toast(now, "cancelled");
        s_flavor = UA_PROMPT;          /* don't arm the connect */
    }
    unlock_finish(now);
}

static void start_worker(uint64_t now, uint8_t op)
{
    memcpy(s_try, s_unlock.code, sizeof(s_try));
    wipe_entry();
    s_op   = op;
    s_done = false;
    s_unlock.deriving = true;
    if (xTaskCreatePinnedToCore(unlock_worker, "ks_unlock", 8192, NULL,
                                5, NULL, 0) != pdPASS) {
        memset(s_try, 0, sizeof(s_try));
        memset(s_old, 0, sizeof(s_old));
        s_unlock.deriving = false;
        toast_for(now, ERR_TOAST_MS, "unlock worker failed");
        enter_home(now);
        return;
    }
    app.next_anim = 0;
    nav_invalidate();
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
    s_unlock.mode        = mode;
    s_unlock.expected    = expected;
    s_unlock.press_until = 0;       /* don't carry a lit pad key over */
    app.next_anim = 0;
    nav_invalidate();
}

static void submit(uint64_t now)
{
    if (s_unlock.len == 0 || s_unlock.deriving) return;
    /* Current-code phases refuse to burn a derivation during the wait. */
    if ((s_unlock.mode == UM_UNLOCK || s_unlock.mode == UM_OLD ||
         s_unlock.mode == UM_REMOVE) && keystore_backoff_ms() > 0) {
        wipe_entry();
        flash_backoff(now);
        return;
    }
    switch (s_unlock.mode) {
    case UM_OLD:                       /* stash, then ask for the new code */
        memcpy(s_old, s_unlock.code, sizeof(s_old));
        enter_phase(UM_NEW, 0);
        return;
    case UM_NEW:
        if (s_unlock.len < CODE_MIN) {
            flash_note(now, "AT LEAST 4 CHARACTERS");
            return;                    /* entry kept — extend it */
        }
        memcpy(s_staged, s_unlock.code, sizeof(s_staged));
        enter_phase(UM_CONFIRM, staged_expected());
        return;
    case UM_CONFIRM:
        if (strcmp(s_unlock.code, s_staged) != 0) {
            memset(s_staged, 0, sizeof(s_staged));
            enter_phase(UM_NEW, 0);
            flash_note(now, "CODES DON'T MATCH");
            return;
        }
        memcpy(s_unlock.code, s_staged, sizeof(s_unlock.code));
        memset(s_staged, 0, sizeof(s_staged));
        start_worker(now, s_unlock.creating ? OP_CREATE : OP_CHANGE);
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
    s_unlock.note_until = 0;
    if (s_unlock.len >= (int)sizeof(s_unlock.code) - 1) return;
    s_unlock.code[s_unlock.len++] = c;
    /* Echo the newest character briefly — only while DEFINING a code
     * (typo insurance); unlock/old entry stays fully masked. */
    if (s_unlock.mode == UM_NEW || s_unlock.mode == UM_CONFIRM) {
        s_unlock.reveal_ch    = c;
        s_unlock.reveal_until = now + REVEAL_MS;
    }
    if (s_unlock.expected && s_unlock.len == s_unlock.expected) {
        submit(now);
        return;
    }
    nav_invalidate();
}

static void erase_char(void)
{
    s_unlock.note_until   = 0;
    s_unlock.reveal_ch    = 0;
    s_unlock.reveal_until = 0;
    if (s_unlock.len > 0)
        s_unlock.code[--s_unlock.len] = '\0';
    nav_invalidate();
}

static void unlock_enter(intptr_t arg, uint64_t now)
{
    s_flavor = (uint8_t)arg;
    s_unlock.last_input = now;
    s_unlock.gate     = (s_flavor == UA_GATE);
    s_unlock.creating = false;
    s_unlock.deriving = false;
    s_unlock.note_until = 0;
    switch (s_flavor) {
    case UA_SETPIN:
        s_unlock.creating = keystore_state() == KEYSTORE_ABSENT;
        if (s_unlock.creating) enter_phase(UM_NEW, 0);
        else                     enter_phase(UM_OLD, keystore_pin_len());
        break;
    case UA_REMOVE:
        enter_phase(UM_REMOVE, keystore_pin_len());
        break;
    default:
        enter_phase(UM_UNLOCK, keystore_pin_len());
        break;
    }
}

/* Every departure wipes the transient entry, however it happens. */
static void unlock_exit(uint64_t now)
{
    (void)now;
    wipe_entry();
    memset(s_staged, 0, sizeof(s_staged));
    memset(s_old, 0, sizeof(s_old));
}

void unlock_open(uint64_t now, bool resume_connect)
{
    intptr_t arg = resume_connect ? UA_RESUME : UA_PROMPT;
    if (nav_current() == SCR_CONNECTING) nav_replace(SCR_UNLOCK, arg, now);
    else                                 nav_push(SCR_UNLOCK, arg, now);
}

void unlock_open_gate(uint64_t now)
{
    nav_reset(SCR_UNLOCK, UA_GATE, now);   /* nothing behind the gate */
}

void unlock_open_setpin(uint64_t now)
{
    nav_push(SCR_UNLOCK, UA_SETPIN, now);
}

void unlock_open_remove(uint64_t now)
{
    nav_push(SCR_UNLOCK, UA_REMOVE, now);
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
            /* Secrets just became readable: re-read profiles so the RAM
             * copies hydrate their diverted passwords, fold any credential
             * a past firmware left in the driver's NVS into the bundle,
             * and kick WiFi — with driver persistence retired, this is
             * where the deck first gets a usable PSK (pre-shared key). */
            load_profiles();
            wifi_migrate_nvs_cred();
            if (!wifi_manager_is_connected()) kick_wifi();
            toast(now, s_unlock.gate ? "deck unlocked"
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

static void unlock_tick(uint64_t now)
{
    if (s_unlock.deriving && s_done) {
        s_unlock.deriving   = false;
        s_unlock.last_input = now;   /* fresh idle window after a bounce */
        wipe_entry();
        worker_result(now, (esp_err_t)s_result);
        if (nav_current() != SCR_UNLOCK) return;
    }
    if (s_unlock.note_until && now >= s_unlock.note_until)
        s_unlock.note_until = 0;
    if (s_unlock.press_until && now >= s_unlock.press_until)
        s_unlock.press_until = 0;
    if (s_unlock.reveal_until && now >= s_unlock.reveal_until) {
        s_unlock.reveal_until = 0;
        s_unlock.reveal_ch    = 0;
    }

    /* An unattended pad must not sit lit forever, and the two pad kinds
     * resolve that differently. The DEVICE gate cannot cancel (that would
     * be a bypass): the rain falls over it and wake lands right back on
     * the pad. A cancellable pad — connect fallback, abandoned set-code
     * flow — gives up after a minute and lands on HOME. */
    if (!s_unlock.deriving) {
        if (s_unlock.gate) {
            if (saver_tick_gate(now)) return;   /* rain owns the screen */
        } else if (now - s_unlock.last_input >= IDLE_CANCEL_MS) {
            /* An abandoned pad always lands HOME, even a menu flow. */
            if (s_flavor == UA_SETPIN || s_flavor == UA_REMOVE)
                s_flavor = UA_PROMPT;
            unlock_cancel(now);
            return;
        }
    }

    if (now >= app.next_anim) {                     /* spinner/caret/blink */
        app.next_anim = now + ANIM_PERIOD_MS;
        nav_invalidate();
    }
}

/* Light the pad tile under a finger for PRESS_MS — set BEFORE dispatch so
 * the action's own render already shows it. Touch only: the finger has
 * already given the digit away to anyone watching, a keystroke has not. */
static void pad_flash(int slot, uint64_t now)
{
    s_unlock.press       = (int8_t)slot;
    s_unlock.press_until = now + PRESS_MS;
}

static void unlock_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                         uint64_t now)
{
    s_unlock.last_input = now;           /* any touch/key holds the pad */
    if (s_unlock.deriving) return;       /* KDF running: not cancellable */

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

    /* No pad_flash() here on purpose — a keyboard press is not visible on
     * the deck, so echoing it as a lit tile would leak the code to the
     * room. The entry row alone acknowledges the keystroke. */
    if (k == K_ESC)            unlock_cancel(now);
    else if (k == K_ENTER)     submit(now);
    else if (k == K_BACKSPACE) erase_char();
    else if (k == K_CHAR)      append_char(ch, now);
}

const nav_screen_t unlock_screen = {
    .name = "unlock", .enter = unlock_enter, .exit = unlock_exit,
    .tick = unlock_tick, .input = unlock_input, .render = render_unlock,
    .chrome = NAV_CHROME_FULL,
};
