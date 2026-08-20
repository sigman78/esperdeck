/*
 * sim/main.c — host composition root.
 *
 * SDL event pumping and host key translation live here; the shell
 * (profile picker, session flow, menus) is the same cyberdeck_app code
 * that runs on the device. BLE is not available on the host (ble = NULL);
 * the SDL keyboard stands in for the BLE keyboard.
 *
 * Controls: F12 = in-session menu, Alt+Enter = window scale,
 *           left-click = touch (tap on release < 300 ms, long-press when
 *           held >= 500 ms — GT911 timing), right-click = instant long-press.
 */

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#include "cyberdeck_app.h"
#include "display.h"
#include "display_fx.h"
#include "font.h"
#include "keystore_cli.h"
#include "ssh_client.h"
#include "storage.h"
#include "vterm.h"
#include "vtkeys.h"
#include "wifi_manager.h"


/* SDL keysym → logical key; the byte sequences live in vtkeys.c, shared
 * with the device's HID backend so the two backends cannot drift. */
static vtkey_t sdl_to_vtkey(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_UP:        return VTKEY_UP;
    case SDLK_DOWN:      return VTKEY_DOWN;
    case SDLK_LEFT:      return VTKEY_LEFT;
    case SDLK_RIGHT:     return VTKEY_RIGHT;
    case SDLK_HOME:      return VTKEY_HOME;
    case SDLK_END:       return VTKEY_END;
    case SDLK_INSERT:    return VTKEY_INSERT;
    case SDLK_DELETE:    return VTKEY_DELETE;
    case SDLK_PAGEUP:    return VTKEY_PGUP;
    case SDLK_PAGEDOWN:  return VTKEY_PGDN;
    case SDLK_F1:        return VTKEY_F1;
    case SDLK_F2:        return VTKEY_F2;
    case SDLK_F3:        return VTKEY_F3;
    case SDLK_F4:        return VTKEY_F4;
    case SDLK_F5:        return VTKEY_F5;
    case SDLK_F6:        return VTKEY_F6;
    case SDLK_F7:        return VTKEY_F7;
    case SDLK_F8:        return VTKEY_F8;
    case SDLK_F9:        return VTKEY_F9;
    case SDLK_F10:       return VTKEY_F10;
    case SDLK_F11:       return VTKEY_F11;
    case SDLK_F12:       return VTKEY_F12;
    default:             return VTKEY_NONE;
    }
}

/* SDL modifier state → the three modifiers the wire format can carry. */
static uint8_t sdl_to_vtmods(SDL_Keymod mod)
{
    return (uint8_t)(((mod & KMOD_SHIFT) ? VTMOD_SHIFT : 0u) |
                     ((mod & KMOD_ALT)   ? VTMOD_ALT   : 0u) |
                     ((mod & KMOD_CTRL)  ? VTMOD_CTRL  : 0u));
}

/*
 * Translate an SDL keydown event to a terminal escape sequence.
 * Returns NULL for printable characters (handled by SDL_TEXTINPUT).
 */
static const char *translate_key(SDL_Keycode sym, SDL_Keymod mod)
{
    if ((mod & KMOD_CTRL) && sym >= SDLK_a && sym <= SDLK_z) {
        static char ctrl_buf[2];
        ctrl_buf[0] = (char)(sym - SDLK_a + 1);
        ctrl_buf[1] = '\0';
        return ctrl_buf;
    }

    switch (sym) {
    case SDLK_RETURN:    return "\r";
    case SDLK_KP_ENTER:  return "\r";
    case SDLK_BACKSPACE: return "\x7f";
    case SDLK_TAB:       return "\t";
    case SDLK_ESCAPE:    return "\x1b";
    default:             break;
    }

    vtkey_t key = sdl_to_vtkey(sym);
    if (key == VTKEY_NONE) return NULL;

    /* Encoded sequences never contain NUL, so a NUL-terminated static
     * buffer keeps the caller's strlen() contract intact. */
    static char seq[VTKEYS_MAX_LEN + 1];
    size_t n = vtkeys_encode(key, sdl_to_vtmods(mod), vterm_app_cursor_keys(),
                             (uint8_t *)seq, VTKEYS_MAX_LEN);
    if (n == 0) return NULL;
    seq[n] = '\0';
    return seq;
}

static void send_key_bytes(const char *seq, size_t len, uint64_t now)
{
    if (!seq || len == 0 || len > 8) return;
    cyberdeck_input_t ev = { .type = CYBERDECK_INPUT_KEY, .len = (uint8_t)len };
    memcpy(ev.buf, seq, len);
    cyberdeck_app_handle_input(&ev, now);
}

/*
 * Mouse → touch emulation, mirroring the GT911 state machine in
 * touch_input.c: tap = press + release within TAP_MAX_MS, long-press fires
 * while still held at LONG_PRESS_MS, a release in between lands in the dead
 * zone and posts nothing. Events carry the press-down position, mapped from
 * window to framebuffer space (the window may be scaled or resized).
 * Right-click skips the hold and posts a long-press immediately.
 */
#define TOUCH_TAP_MAX_MS    300
#define TOUCH_LONG_PRESS_MS 500
/* Matches SCROLL_START_PX in touch_input.c — the travel that turns an edge
 * press into a drag rather than a tap. */
#define TOUCH_SCROLL_START_PX 12

typedef enum {
    TOUCH_IDLE, TOUCH_TOUCHING, TOUCH_SCROLLING, TOUCH_WAITING_LIFT
} touch_state_t;

static touch_state_t touch_state = TOUCH_IDLE;
static uint64_t      touch_start;
static uint16_t      touch_x, touch_y;

static void send_touch(uint8_t type, uint16_t x, uint16_t y, uint64_t now)
{
    cyberdeck_input_t ev = { .type = type, .x = x, .y = y };
    cyberdeck_app_handle_input(&ev, now);
}

static void touch_mouse_down(const SDL_MouseButtonEvent *b, uint64_t now)
{
    uint16_t x, y;
    display_window_to_fb(b->x, b->y, &x, &y);

    if (b->button == SDL_BUTTON_RIGHT) {
        send_touch(CYBERDECK_INPUT_LONG_PRESS, x, y, now);
        return;
    }
    if (b->button != SDL_BUTTON_LEFT || touch_state != TOUCH_IDLE) return;

    SDL_CaptureMouse(SDL_TRUE);  /* see the release even outside the window */
    touch_x     = x;
    touch_y     = y;
    touch_start = now;
    touch_state = TOUCH_TOUCHING;
}

static void touch_mouse_up(const SDL_MouseButtonEvent *b, uint64_t now)
{
    if (b->button != SDL_BUTTON_LEFT || touch_state == TOUCH_IDLE) return;
    SDL_CaptureMouse(SDL_FALSE);
    if (touch_state == TOUCH_TOUCHING && now - touch_start < TOUCH_TAP_MAX_MS)
        send_touch(CYBERDECK_INPUT_TAP, touch_x, touch_y, now);
    touch_state = TOUCH_IDLE;
}

/* Right-edge scroll drag, mirroring touch_input.c's STATE_SCROLLING: a press
 * that STARTS in the strip and then travels vertically turns into a stream of
 * SCROLL events instead of the tap it would have been. Armed by the shell
 * through sim_set_scroll_edge (the cfg.set_scroll_edge seam), so the menu
 * toggle governs the simulator exactly as it governs the panel. */
static int      s_scroll_edge_px = 0;
static uint16_t s_scroll_last_y;

static void sim_set_scroll_edge(int width_px)
{
    s_scroll_edge_px = width_px < 0 ? 0 : width_px;
}

/* Mouse wheel → scroll, sim-only. The panel has no wheel, so this rides the
 * same SCROLL event as the edge drag rather than inventing a second path:
 * one notch is reported as a cell of travel, which the shell then scales by
 * the configured drag speed exactly as it does a finger. SDL gives positive
 * y for a push away from you, which should show older lines — the same
 * direction as dragging the content down. */
static void mouse_wheel(const SDL_MouseWheelEvent *w, uint64_t now)
{
    if (!w->y) return;
    int notches = w->y;
#if SDL_VERSION_ATLEAST(2, 0, 4)
    if (w->direction == SDL_MOUSEWHEEL_FLIPPED) notches = -notches;
#endif
    cyberdeck_input_t ev = {
        .type = CYBERDECK_INPUT_SCROLL,
        .dy   = (int16_t)(notches * font_height()),
    };
    cyberdeck_app_handle_input(&ev, now);
}

static void touch_mouse_motion(const SDL_MouseMotionEvent *m, uint64_t now)
{
    if (touch_state != TOUCH_TOUCHING && touch_state != TOUCH_SCROLLING) return;

    uint16_t x, y;
    display_window_to_fb(m->x, m->y, &x, &y);

    if (touch_state == TOUCH_TOUCHING) {
        if (s_scroll_edge_px <= 0 ||
            (int)touch_x < DISPLAY_WIDTH - s_scroll_edge_px) return;
        int dy = (int)y - (int)touch_y;
        if (dy > -TOUCH_SCROLL_START_PX && dy < TOUCH_SCROLL_START_PX) return;
        s_scroll_last_y = y;
        touch_state     = TOUCH_SCROLLING;
        return;
    }

    if (y == s_scroll_last_y) return;
    cyberdeck_input_t ev = {
        .type = CYBERDECK_INPUT_SCROLL,
        .x    = x,
        .y    = y,
        .dy   = (int16_t)((int)y - (int)s_scroll_last_y),
    };
    s_scroll_last_y = y;
    cyberdeck_app_handle_input(&ev, now);
}

static void touch_tick(uint64_t now)
{
    if (touch_state == TOUCH_TOUCHING &&
        now - touch_start >= TOUCH_LONG_PRESS_MS) {
        send_touch(CYBERDECK_INPUT_LONG_PRESS, touch_x, touch_y, now);
        touch_state = TOUCH_WAITING_LIFT;
    }
}

/* -------------------------------------------------------------------------
 * --drive "tap:x,y|key:enter|hold:x,y|wait:800" — scripted input for UI
 * screenshot automation (framebuffer pixel coords; key names: enter, esc,
 * tab, up/down/left/right, f12, sbup/sbdn for scrollback paging, or a
 * single character). Steps fire 450 ms
 * apart (wait:N overrides the gap) starting 2.5 s after boot, injected
 * through the same paths as real input. Sim-only test hook.
 * ---------------------------------------------------------------------- */
static const char *s_drive      = NULL;
static uint64_t    s_drive_next = 0;

static void drive_key(const char *name, uint64_t now)
{
    const char *seq = NULL;
    char one[2] = { 0, 0 };
    if      (!strcmp(name, "enter")) seq = "\r";
    else if (!strcmp(name, "esc"))   seq = "\x1b";
    else if (!strcmp(name, "tab"))   seq = "\t";
    else if (!strcmp(name, "up"))    seq = "\x1b[A";
    else if (!strcmp(name, "down"))  seq = "\x1b[B";
    else if (!strcmp(name, "right")) seq = "\x1b[C";
    else if (!strcmp(name, "left"))  seq = "\x1b[D";
    else if (!strcmp(name, "f12"))   seq = "\x1b[24~";
    /* Scrollback paging — the deck keeps these rather than forwarding. */
    else if (!strcmp(name, "sbup"))  seq = "\x1b[5;2~";
    else if (!strcmp(name, "sbdn"))  seq = "\x1b[6;2~";
    else if (name[0] && !name[1])    { one[0] = name[0]; seq = one; }
    if (seq) send_key_bytes(seq, strlen(seq), now);
}

static void drive_tick(uint64_t now)
{
    if (!s_drive || !*s_drive || now < s_drive_next) return;
    char step[64];
    const char *bar = strchr(s_drive, '|');
    size_t n = bar ? (size_t)(bar - s_drive) : strlen(s_drive);
    if (n >= sizeof(step)) n = sizeof(step) - 1;
    memcpy(step, s_drive, n);
    step[n] = '\0';
    s_drive = bar ? bar + 1 : "";

    uint64_t gap = 450;
    int x = 0, y = 0;
    if (!strncmp(step, "wait:", 5)) {
        gap = strtoul(step + 5, NULL, 10);
    } else if (sscanf(step, "tap:%d,%d", &x, &y) == 2) {
        send_touch(CYBERDECK_INPUT_TAP, (uint16_t)x, (uint16_t)y, now);
    } else if (sscanf(step, "hold:%d,%d", &x, &y) == 2) {
        send_touch(CYBERDECK_INPUT_LONG_PRESS, (uint16_t)x, (uint16_t)y, now);
    } else if (!strncmp(step, "key:", 4)) {
        drive_key(step + 4, now);
    }
    s_drive_next = now + gap;
}

int main(int argc, char *argv[])
{
    /* Keystore provisioning commands run headless and exit (see
     * keystore_cli.c); returns -1 when none is present. */
    int cli_rc = keystore_cli_main(argc, argv);
    if (cli_rc >= 0) return cli_rc;

    /* --drive <script> is consumed first (scripted-input test hook); the
     * remaining positionals: host [port [user [password]]] become the
     * "(default)" entry in the profile picker. */
    if (argc > 2 && strcmp(argv[1], "--drive") == 0) {
        s_drive      = argv[2];
        s_drive_next = SDL_GetTicks64() + 2500;
        argv += 2;
        argc -= 2;
    }
    const char *host = (argc > 1) ? argv[1] : "";
    int         port = (argc > 2) ? atoi(argv[2]) : 22;
    const char *user = (argc > 3) ? argv[3] : "user";
    const char *pass = (argc > 4) ? argv[4] : "";

#ifdef _WIN32
    /* Log strings are UTF-8 (em dashes etc.); the console default is the
     * OEM codepage, which renders them as mojibake ("ΓÇö"). */
    SetConsoleOutputCP(CP_UTF8);
#endif

    if (storage_init() != ESP_OK) {
        fprintf(stderr, "storage_init() failed\n");
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* The sim links exactly one size (cmake -DFONT_SIZE=...), so the request
     * is irrelevant — font_init falls back to whichever one that is. The
     * renderer still has to be told, or it keeps its 8x16 defaults. */
    font_init(FONT_SIZE_8X16);
    display_render_set_font(font_width(), font_height());
    display_init();
    vterm_init(display_text_cols(), display_text_rows());
    vterm_write("\x1b[2J\x1b[H", 7);
    display_render_frame();

    wifi_manager_init();
    ssh_client_init();

    cyberdeck_app_config_t app_cfg = {
        .boot_delay_ms      = 1500,
        .ssh_retry_delay_ms = 5000,
        .auto_reconnect     = true,
        .version            = "sim",
        .fallback_host      = host,
        .fallback_port      = (uint16_t)port,
        .fallback_user      = user,
        .fallback_password  = pass,
        .fallback_wifi_ssid     = "SIM",
        .fallback_wifi_password = "",
        .ble = NULL,
#if CONFIG_INPUT_TOUCH_SCROLL
        .set_scroll_edge = sim_set_scroll_edge,
        .scroll_edge_px  = CONFIG_INPUT_TOUCH_SCROLL_EDGE_PX,
#endif
    };
    if (cyberdeck_app_init(&app_cfg, SDL_GetTicks64()) != ESP_OK) {
        fprintf(stderr, "cyberdeck_app_init() failed\n");
        SDL_Quit();
        return 1;
    }

    bool running = true;
    while (running) {
        bool got_input = false;
        uint64_t now = SDL_GetTicks64();

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                running = false;
                break;

            case SDL_KEYDOWN: {
                if ((ev.key.keysym.mod & KMOD_ALT) &&
                    (ev.key.keysym.sym == SDLK_RETURN ||
                     ev.key.keysym.sym == SDLK_KP_ENTER)) {
                    display_toggle_scale();
                    break;
                }
                /* Sim-only fx previews (no SSH events on the host):
                 * Ctrl+F5 wipe, Ctrl+F6 collapse, Ctrl+F7 static burst. */
                if (ev.key.keysym.mod & KMOD_CTRL) {
                    if (ev.key.keysym.sym == SDLK_F5) { display_fx_wipe();     break; }
                    if (ev.key.keysym.sym == SDLK_F6) { display_fx_collapse(); break; }
                    if (ev.key.keysym.sym == SDLK_F7) { display_fx_static();   break; }
                }
                const char *seq = translate_key(ev.key.keysym.sym,
                                                ev.key.keysym.mod);
                if (seq) send_key_bytes(seq, strlen(seq), now);
                got_input = true;
                break;
            }

            case SDL_TEXTINPUT:
                send_key_bytes(ev.text.text, strlen(ev.text.text), now);
                got_input = true;
                break;

            case SDL_MOUSEMOTION:
                touch_mouse_motion(&ev.motion, now);
                break;

            case SDL_MOUSEWHEEL:
                mouse_wheel(&ev.wheel, now);
                got_input = true;
                break;

            case SDL_MOUSEBUTTONDOWN:
                touch_mouse_down(&ev.button, now);
                got_input = true;
                break;

            case SDL_MOUSEBUTTONUP:
                touch_mouse_up(&ev.button, now);
                got_input = true;
                break;

            default:
                break;
            }
        }

        uint64_t tick = SDL_GetTicks64();
        touch_tick(tick);
        drive_tick(tick);
        cyberdeck_app_tick(tick);
        display_render_frame();

        SDL_Delay(got_input ? 1 : 16);
    }

    ssh_client_disconnect();
    SDL_Quit();
    return 0;
}
