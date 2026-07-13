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
#include "font.h"
#include "ssh_client.h"
#include "storage.h"
#include "vterm.h"
#include "wifi_manager.h"

#ifndef CONFIG_TERMINAL_WIDTH
#define CONFIG_TERMINAL_WIDTH  100
#endif
#ifndef CONFIG_TERMINAL_HEIGHT
#define CONFIG_TERMINAL_HEIGHT 30
#endif

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

    case SDLK_UP:    return vterm_app_cursor_keys() ? "\x1bOA" : "\x1b[A";
    case SDLK_DOWN:  return vterm_app_cursor_keys() ? "\x1bOB" : "\x1b[B";
    case SDLK_LEFT:  return vterm_app_cursor_keys() ? "\x1bOD" : "\x1b[D";
    case SDLK_RIGHT: return vterm_app_cursor_keys() ? "\x1bOC" : "\x1b[C";

    case SDLK_HOME:      return "\x1b[H";
    case SDLK_END:       return "\x1b[F";
    case SDLK_PAGEUP:    return "\x1b[5~";
    case SDLK_PAGEDOWN:  return "\x1b[6~";
    case SDLK_DELETE:    return "\x1b[3~";
    case SDLK_INSERT:    return "\x1b[2~";

    case SDLK_F1:        return "\x1bOP";
    case SDLK_F2:        return "\x1bOQ";
    case SDLK_F3:        return "\x1bOR";
    case SDLK_F4:        return "\x1bOS";
    case SDLK_F5:        return "\x1b[15~";
    case SDLK_F6:        return "\x1b[17~";
    case SDLK_F7:        return "\x1b[18~";
    case SDLK_F8:        return "\x1b[19~";
    case SDLK_F9:        return "\x1b[20~";
    case SDLK_F10:       return "\x1b[21~";
    case SDLK_F11:       return "\x1b[23~";
    case SDLK_F12:       return "\x1b[24~";

    default:             return NULL;
    }
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

typedef enum { TOUCH_IDLE, TOUCH_TOUCHING, TOUCH_WAITING_LIFT } touch_state_t;

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

static void touch_tick(uint64_t now)
{
    if (touch_state == TOUCH_TOUCHING &&
        now - touch_start >= TOUCH_LONG_PRESS_MS) {
        send_touch(CYBERDECK_INPUT_LONG_PRESS, touch_x, touch_y, now);
        touch_state = TOUCH_WAITING_LIFT;
    }
}

int main(int argc, char *argv[])
{
    /* Optional argv override: host [port [user [password]]] becomes the
     * "(default)" entry in the profile picker. */
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

    font_init();
    display_init();
    vterm_init(CONFIG_TERMINAL_WIDTH, CONFIG_TERMINAL_HEIGHT);
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
        cyberdeck_app_tick(tick);
        display_render_frame();

        SDL_Delay(got_input ? 1 : 16);
    }

    ssh_client_disconnect();
    SDL_Quit();
    return 0;
}
