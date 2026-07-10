/*
 * display_sdl.c — SDL2 display backend for the PC simulator.
 *
 * Implements the same display.h public API as lcd_driver.c, using SDL2
 * instead of the ESP32 RGB panel.  All pixel rendering is delegated to
 * display_render_chunk (display_render.c) — no rendering logic here.
 *
 * Compiled only when BUILD_SIMULATOR is defined.
 */

#ifdef BUILD_SIMULATOR

#include "display.h"
#include "display_render.h"
#include "font.h"
#include "esp_log.h"
#include <SDL2/SDL.h>
#include <stdint.h>

static const char *TAG = "display_sdl";

static SDL_Window   *s_window   = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture  = NULL;
static int           s_scale    = 1;   /* 1 or 2 */

/* -------------------------------------------------------------------------
 * display.h public API
 * ---------------------------------------------------------------------- */

esp_err_t display_init(void)
{
    ESP_LOGI(TAG, "Initializing SDL2 display (%dx%d)", DISPLAY_WIDTH, DISPLAY_HEIGHT);

    s_window = SDL_CreateWindow(
        "unbreezy cyberdeck — simulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        DISPLAY_WIDTH, DISPLAY_HEIGHT,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!s_window) {
        ESP_LOGE(TAG, "SDL_CreateWindow failed: %s", SDL_GetError());
        return ESP_FAIL;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        ESP_LOGE(TAG, "SDL_CreateRenderer failed: %s", SDL_GetError());
        return ESP_FAIL;
    }

    /* Streaming RGB565 texture — matches the ESP32 framebuffer format exactly. */
    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_RGB565,
        SDL_TEXTUREACCESS_STREAMING,
        DISPLAY_WIDTH, DISPLAY_HEIGHT);
    if (!s_texture) {
        ESP_LOGE(TAG, "SDL_CreateTexture failed: %s", SDL_GetError());
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "SDL2 display ready");
    return ESP_OK;
}

esp_err_t display_set_backlight(uint8_t brightness)
{
    /* No-op in the simulator. */
    (void)brightness;
    return ESP_OK;
}

/* -------------------------------------------------------------------------
 * Simulator-only: render one full frame using display_render_chunk.
 * Called from sim/main.c each iteration of the event loop.
 * ---------------------------------------------------------------------- */

void display_render_frame(void)
{
    void *pixels = NULL;
    int   pitch  = 0;

    if (SDL_LockTexture(s_texture, NULL, &pixels, &pitch) != 0) {
        ESP_LOGE(TAG, "SDL_LockTexture failed: %s", SDL_GetError());
        return;
    }

    /* Render one character-row band per call to display_render_chunk,
     * mirroring how the ESP32 ISR calls it for each bounce-buffer fill. */
    const int chunk_px    = DISPLAY_WIDTH * FONT_HEIGHT;
    const int chunk_bytes = chunk_px * (int)sizeof(color_t);
    const int num_rows    = DISPLAY_HEIGHT / FONT_HEIGHT;

    for (int r = 0; r < num_rows; r++) {
        display_render_chunk(
            (color_t *)pixels + r * chunk_px,
            r * chunk_px,
            chunk_bytes);
    }

    SDL_UnlockTexture(s_texture);
    SDL_RenderCopy(s_renderer, s_texture, NULL, NULL);
    SDL_RenderPresent(s_renderer);
}

void display_toggle_scale(void)
{
    s_scale = (s_scale == 1) ? 2 : 1;
    SDL_SetWindowSize(s_window, DISPLAY_WIDTH * s_scale, DISPLAY_HEIGHT * s_scale);
    SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

#endif /* BUILD_SIMULATOR */
