/*
 * app_unlock.c - the access code pad (SCR_UNLOCK): unlock, set, change
 * and remove the keystore code. Model: docs/storage_auth.md (two gates).
 * A derivation runs on a worker task (Argon2id, about 1 s) and never in
 * a UI tick. The pad ignores input meanwhile.
 */

#include "app_internal.h"
#include "app_menu_defs.h"   /* pulls in MS_KEYSTORE, the menu return target */
#include "app_screens.h"
#include "app_widgets.h"
#include "display_fx.h"
#include "keystore.h"
#include "ui_pure.h"         /* code row plan, UTF-8 put */
#include "wifi_manager.h"    /* post-unlock PSK re-kick */

#include "esp_log.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_unlock";

static struct {
    /* code holds the typed PIN, sized to KEYSTORE_PIN_MAX + 1. It lives in
     * .bss, so it lands in internal SRAM. Submit or leaving the screen
     * wipes it. */
    char     code[65];
    int      len;
    uint8_t  expected;              /* auto-submit length (0 = Enter only) */
    uint8_t  mode;                  /* entry phase (um_mode)               */
    bool     creating;              /* set-code flow on an ABSENT store    */
    bool     deriving;              /* KDF worker running; input swallowed */
    const char *note;               /* shown until the next input (static) */
    uint64_t last_input;            /* idle-cancel clock (IDLE_CANCEL_MS)  */
    char     reveal_ch;             /* newest typed char, echoed briefly...*/
    uint64_t reveal_until;          /* ...while defining a code (0 = off)  */
    /* In the two-gates model, a keystore on the deck locks the deck.
     * The gate field marks this pad as the DEVICE gate (boot/wake):
     * non-skippable, no idle-cancel, and the saver rains over it. */
    bool     gate;
} s_unlock;

#define CODE_MIN     4            /* UI floor for a new code's length */
#define REVEAL_MS   800          /* newest char shown while defining */
#define IDLE_CANCEL_MS 60000      /* unattended pad cancels to HOME   */
#define CLOSE_QUIET_MS 600        /* taps after the pad closes drop   */

/* Entry phase (s_unlock.mode). */
enum { UM_UNLOCK = 0, UM_OLD, UM_NEW, UM_CONFIRM, UM_REMOVE };

/* Intent arg (nav): why this pad is up. Where to land when it closes is
 * the nav stack's business - except the connect continuation. */
enum { UA_PROMPT = 0, UA_RESUME, UA_GATE, UA_SETPIN, UA_REMOVE };
static uint8_t s_flavor;

/* Worker op - the keystore call made on the worker task. */
enum { OP_UNLOCK = 0, OP_CREATE, OP_CHANGE, OP_REMOVE };

/* Worker exchange + flow stashes - .bss (internal SRAM), wiped as soon as
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

/* Pad keys: 1 2 3 / 4 5 6 / 7 8 9 / erase 0 OK. Region ids are ID_KEY + slot. */
enum { ID_KEY = 1, ID_NOFOCUS = 100 };
#define SLOT_ERASE 9
#define SLOT_OK    11

static const char *const PAD_LBL[12] = { "1", "2", "3", "4", "5", "6", "7",
                                         "8", "9", UI_S_LEFT, "0", "OK" };

static void phase_head(const char **title, const char **note)
{
    switch (s_unlock.mode) {
    case UM_OLD:
        *title = "Change code";
        *note  = "Enter the current code";
        break;
    case UM_NEW:
    case UM_CONFIRM:
        *title = s_unlock.creating ? "Create keystore" : "Change code";
        *note  = s_unlock.mode == UM_NEW ? "Choose a new code, 4 or more"
                                         : "Enter the new code again";
        break;
    case UM_REMOVE:
        *title = "Remove code";
        *note  = "Enter the code to remove it";
        break;
    default:
        *title = s_unlock.gate ? "Deck locked" : "Keystore locked";
        *note  = s_flavor == UA_RESUME ? "Enter the access code to connect"
                                       : "Enter the access code";
        break;
    }
}

/* Marks of the typed code on the entry line. While defining a code the
 * newest character shows itself. */
static void draw_code_row(ui_rect_t line)
{
    const ui_code_t c = ui_code_plan(s_unlock.len, s_unlock.expected, line.w);
    const int step = c.spaced ? 2 : 1;
    int x = line.x + (line.w - c.width) / 2;

    ui_pen(OVERLAY_COL_DEFAULT);
    for (int i = 0; i < c.bullets + c.dots; i++, x += step) {
        if (i >= c.bullets)
            ui_putch(x, line.y, 0xB7, UI_PANEL_MUTED);
        else if (i == c.bullets - 1 && s_unlock.reveal_until)
            ui_putch(x, line.y, (uint16_t)(uint8_t)s_unlock.reveal_ch,
                     UI_PANEL | UI_BOLD);
        else
            ui_putch(x, line.y, 0x2022, UI_PANEL | UI_BOLD);
    }
}

static void draw_entry_panel(ui_rect_t entry)
{
    const ui_rect_t line = ui_rect(entry.x + 2, entry.y + 1, entry.w - 4, 1);
    const uint32_t wait_ms = keystore_backoff_ms();
    char msg[64];

    ui_panel(entry, s_unlock.note && !s_unlock.deriving ? OVERLAY_COL_RED : 0);
    if (s_unlock.deriving) {
        char glyph[8];
        ui_utf8_put(glyph, spinner_glyph(app.anim_frame));
        snprintf(msg, sizeof msg, "%s Checking", glyph);
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_text(line, msg, UI_PANEL | UI_BOLD, UI_CENTER);
    } else if (s_unlock.note) {
        /* The wait stays visible beside a note, counted live. */
        if (wait_ms > 0 && s_unlock.len == 0)
            snprintf(msg, sizeof msg, "%s, wait %u s", s_unlock.note,
                     (unsigned)((wait_ms + 999) / 1000));
        else
            snprintf(msg, sizeof msg, "%s", s_unlock.note);
        ui_pen(OVERLAY_COL_RED);
        ui_text(line, msg, UI_PANEL | UI_BOLD, UI_CENTER);
    } else if (wait_ms > 0 && s_unlock.len == 0) {
        snprintf(msg, sizeof msg, "Wait %u s", (unsigned)((wait_ms + 999) / 1000));
        ui_pen(OVERLAY_COL_AMBER);
        ui_text(line, msg, UI_PANEL | UI_BOLD, UI_CENTER);
    } else if (s_unlock.len == 0) {
        ui_pen(OVERLAY_COL_DEFAULT);
        ui_text(line, s_unlock.mode == UM_NEW || s_unlock.mode == UM_CONFIRM
                          ? "new code" : "access code",
                UI_PANEL_MUTED, UI_CENTER);
    } else {
        draw_code_row(line);
    }
    ui_pen(OVERLAY_COL_DEFAULT);
}

static void render_unlock(uint64_t now)
{
    (void)now;
    const int faults = ui_faults();
    const ui_slate_t pg = ui_slate_page();
    const char *title, *note;
    const int kw = 9;

    /* No key holds focus: a lit key would show the typed code. */
    ui_focus_set(ID_NOFOCUS);
    phase_head(&title, &note);
    ui_title(pg.header, NULL, title, note, !s_unlock.gate);

    /* Five-row keys where they fit: the digit stays centred. */
    const int kh = pg.body.h >= 3 + 4 * 5 ? 5 : 3;
    ui_rect_t pad = ui_center(pg.body, 3 * kw + 2, 3 + 4 * kh);
    draw_entry_panel(ui_cut_top(&pad, 3));
    for (int i = 0; i < 12; i++) {
        const ui_rect_t r = ui_rect(pad.x + (i % 3) * (kw + 1),
                                    pad.y + (i / 3) * kh, kw, kh);
        const bool off = (i == SLOT_ERASE || i == SLOT_OK) && !s_unlock.len;
        const uint8_t flags = off ? UI_PART_OFF : 0;
        if (i == SLOT_OK)
            ui_push(r, (ui_id_t)(ID_KEY + i), PAD_LBL[i], UI_TONE_SOLID,
                    OVERLAY_COL_GREEN, flags);
        else
            ui_push(r, (ui_id_t)(ID_KEY + i), PAD_LBL[i], UI_TONE_CARD, 0,
                    flags);
    }
    if (ui_faults() != faults)
        ESP_LOGW(TAG, "layout faults +%d", ui_faults() - faults);
}

static void flash_note(const char *msg)
{
    s_unlock.note = msg;
    app.next_anim = 0;
}

static void flash_backoff(void)
{
    s_unlock.note = NULL;
    app.next_anim = 0;
}

static void wipe_entry(void)
{
    memset(s_unlock.code, 0, sizeof(s_unlock.code));
    s_unlock.len          = 0;
    s_unlock.reveal_ch    = 0;      /* echoes a code char - wipe with it */
    s_unlock.reveal_until = 0;
}

/* Land wherever the caller opened this screen from. */
static void unlock_finish(uint64_t now)
{
    wipe_entry();
    memset(s_staged, 0, sizeof(s_staged));
    /* The pad closes about a second after the last tap (the code check).
     * A finger still tapping there would land on the page below. */
    touch_quiet(now, CLOSE_QUIET_MS);
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
        flash_backoff();
        return;
    }
    switch (s_unlock.mode) {
    case UM_OLD:                       /* stash, then ask for the new code */
        memcpy(s_old, s_unlock.code, sizeof(s_old));
        enter_phase(UM_NEW, 0);
        return;
    case UM_NEW:
        if (s_unlock.len < CODE_MIN) {
            flash_note("At least 4 characters");
            return;                    /* entry kept - extend it */
        }
        memcpy(s_staged, s_unlock.code, sizeof(s_staged));
        enter_phase(UM_CONFIRM, staged_expected());
        return;
    case UM_CONFIRM:
        if (strcmp(s_unlock.code, s_staged) != 0) {
            memset(s_staged, 0, sizeof(s_staged));
            enter_phase(UM_NEW, 0);
            flash_note("The codes do not match");
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
    s_unlock.note = NULL;
    if (s_unlock.len >= (int)sizeof(s_unlock.code) - 1) return;
    s_unlock.code[s_unlock.len++] = c;
    /* Echo the newest character briefly - only while DEFINING a code
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
    s_unlock.note         = NULL;
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
    s_unlock.note = NULL;
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

bool unlock_is_gate(void)
{
    return s_unlock.gate && !s_unlock.deriving;
}

void unlock_open_setpin(uint64_t now)
{
    nav_push(SCR_UNLOCK, UA_SETPIN, now);
}

void unlock_open_remove(uint64_t now)
{
    nav_push(SCR_UNLOCK, UA_REMOVE, now);
}

/* The worker finished - route the result by op. */
static void worker_result(uint64_t now, esp_err_t r)
{
    if (r == KEYSTORE_ERR_BACKOFF) {   /* wait armed mid-flow; nothing ran */
        if (s_op == OP_CHANGE) enter_phase(UM_OLD, keystore_pin_len());
        flash_backoff();
        return;
    }
    if (s_op == OP_UNLOCK) {
        if (r == ESP_OK) {
            /* Secrets just became readable. Re-read profiles so the RAM
             * copies hydrate their diverted passwords. Fold any
             * credential a past firmware left in the driver's NVS into
             * the bundle. Then kick WiFi. Driver persistence no
             * longer runs, so this is where the deck first gets a
             * usable PSK (pre-shared key). */
            load_profiles();
            wifi_migrate_nvs_cred();
            if (!wifi_manager_is_connected()) kick_wifi();
            toast(now, s_unlock.gate ? "deck unlocked"
                                       : "keystore unlocked");
            unlock_finish(now);
        } else if (r == ESP_FAIL) {                 /* wrong code */
            display_bell();
            display_fx_static_brief();
            flash_note(UI_S_CROSS " wrong code");
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
        flash_note(UI_S_CROSS " wrong code");
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
    if (s_unlock.reveal_until && now >= s_unlock.reveal_until) {
        s_unlock.reveal_until = 0;
        s_unlock.reveal_ch    = 0;
    }

    /* An unattended pad must not sit lit forever, and the two pad kinds
     * resolve that differently. The DEVICE gate cannot cancel (that
     * would be a bypass). The rain falls over it, and wake lands right
     * back on the pad. A cancellable pad - connect fallback, abandoned
     * set-code flow - gives up after a minute and lands on HOME. */
    if (!s_unlock.deriving) {
        if (!s_unlock.gate && now - s_unlock.last_input >= IDLE_CANCEL_MS) {
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

static bool is_touch(int type)
{
    return type == CYBERDECK_INPUT_TAP || type == CYBERDECK_INPUT_PRESS ||
           type == CYBERDECK_INPUT_RELEASE ||
           type == CYBERDECK_INPUT_LONG_PRESS;
}

static void unlock_input(const cyberdeck_input_t *ev, ui_key_t k, char ch,
                         uint64_t now)
{
    s_unlock.last_input = now;           /* any touch/key holds the pad */
    if (s_unlock.deriving) return;       /* KDF running: not cancellable */

    if (ev->type != CYBERDECK_INPUT_KEY) {
        if (!is_touch(ev->type)) return;
        const ui_act_t a = app_hot_input(ev, K_NONE);
        if (a.kind == UI_ACT_MOVE || a.kind == UI_ACT_PRESS) nav_invalidate();
        if (a.kind != UI_ACT_PRESS) return;
        if (a.id == UI_ID_BACK) {
            unlock_cancel(now);
            return;
        }
        const int slot = (int)a.id - ID_KEY;
        if (slot < 0 || slot > SLOT_OK) return;
        if (slot == SLOT_ERASE)   erase_char();
        else if (slot == SLOT_OK) submit(now);
        else append_char(slot == 10 ? '0' : (char)('1' + slot), now);
        return;
    }

    /* Keys never reach the hot regions: a lit pad key would show the
     * code to the room. The entry line alone acknowledges a keystroke. */
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
