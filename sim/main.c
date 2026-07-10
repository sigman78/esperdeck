/*
 * sim/main.c — host composition root.
 *
 * SDL event pumping and host key translation live here; the shell
 * (profile picker, session flow, menus) is the same cyberdeck_app code
 * that runs on the device. BLE is not available on the host (ble = NULL);
 * the SDL keyboard stands in for the BLE keyboard.
 *
 * Controls: F12 = in-session menu, Alt+Enter = window scale,
 *           right-click = touch long-press, left-click = tap.
 */

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

int main(int argc, char *argv[])
{
    /* Optional argv override: host [port [user [password]]] becomes the
     * "(default)" entry in the profile picker. */
    const char *host = (argc > 1) ? argv[1] : "";
    int         port = (argc > 2) ? atoi(argv[2]) : 22;
    const char *user = (argc > 3) ? argv[3] : "user";
    const char *pass = (argc > 4) ? argv[4] : "";

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

            case SDL_MOUSEBUTTONDOWN: {
                cyberdeck_input_t tev = {
                    .type = (ev.button.button == SDL_BUTTON_RIGHT)
                            ? CYBERDECK_INPUT_LONG_PRESS
                            : CYBERDECK_INPUT_TAP,
                    .x = (uint16_t)ev.button.x,
                    .y = (uint16_t)ev.button.y,
                };
                cyberdeck_app_handle_input(&tev, now);
                got_input = true;
                break;
            }

            default:
                break;
            }
        }

        cyberdeck_app_tick(SDL_GetTicks64());
        display_render_frame();

        SDL_Delay(got_input ? 1 : 16);
    }

    ssh_client_disconnect();
    SDL_Quit();
    return 0;
}
