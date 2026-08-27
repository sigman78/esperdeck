/*
 * display_sdl.c — SDL2 backend for the PC simulator: same display.h API as
 * lcd_driver.c, all pixel rendering delegated to display_render_chunk.
 * Compiled only when BUILD_SIMULATOR is defined.
 */

#ifdef BUILD_SIMULATOR

#include "display.h"
#include "display_render.h"
#include "font.h"
#include "esp_log.h"
#include <SDL2/SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "display_sdl";

static SDL_Window   *s_window   = NULL;
static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture  = NULL;
static int           s_scale    = 1;   /* 1 or 2 */

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

/* Render one full frame; called from sim/main.c each event-loop pass. */
void display_render_frame(void)
{
    void *pixels = NULL;
    int   pitch  = 0;

    if (SDL_LockTexture(s_texture, NULL, &pixels, &pitch) != 0) {
        ESP_LOGE(TAG, "SDL_LockTexture failed: %s", SDL_GetError());
        return;
    }

    /* Render one bounce-buffer band per call to display_render_chunk,
     * mirroring how the ESP32 ISR calls it for each bounce-buffer fill. */
    const int band_h      = display_band_height();
    const int chunk_px    = DISPLAY_WIDTH * band_h;
    const int chunk_bytes = chunk_px * (int)sizeof(color_t);
    const int num_rows    = DISPLAY_HEIGHT / band_h;

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

/* Render the current frame into a scratch buffer (same chunk renderer the
 * window path uses) and write it as a bottom-up 24-bit BMP. */
esp_err_t display_screenshot_bmp(const char *path)
{
    const int W = DISPLAY_WIDTH, H = DISPLAY_HEIGHT;
    color_t *fb = malloc((size_t)W * H * sizeof(color_t));
    if (!fb) return ESP_ERR_NO_MEM;

    const int band_h   = display_band_height();
    const int chunk_px = W * band_h;
    for (int r = 0; r < H / band_h; r++)
        display_render_chunk(fb + r * chunk_px, r * chunk_px,
                             chunk_px * (int)sizeof(color_t));

    FILE *f = fopen(path, "wb");
    if (!f) { free(fb); return ESP_FAIL; }

    const uint32_t row_bytes = (uint32_t)W * 3;      /* 2400: 4-aligned */
    const uint32_t img_bytes = row_bytes * (uint32_t)H;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t v;
    v = 54 + img_bytes;  memcpy(hdr + 2,  &v, 4);    /* file size   */
    v = 54;              memcpy(hdr + 10, &v, 4);    /* pixel start */
    v = 40;              memcpy(hdr + 14, &v, 4);    /* BITMAPINFO  */
    v = (uint32_t)W;     memcpy(hdr + 18, &v, 4);
    v = (uint32_t)H;     memcpy(hdr + 22, &v, 4);
    hdr[26] = 1;                                     /* planes      */
    hdr[28] = 24;                                    /* bpp         */
    v = img_bytes;       memcpy(hdr + 34, &v, 4);
    fwrite(hdr, 1, sizeof(hdr), f);

    uint8_t *row = malloc(row_bytes);
    if (!row) { fclose(f); free(fb); return ESP_ERR_NO_MEM; }
    for (int y = H - 1; y >= 0; y--) {               /* bottom-up   */
        const color_t *src = fb + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            const uint16_t px = src[x];              /* RGB565      */
            row[x * 3 + 0] = (uint8_t)((px & 0x1F) << 3);        /* B */
            row[x * 3 + 1] = (uint8_t)(((px >> 5) & 0x3F) << 2); /* G */
            row[x * 3 + 2] = (uint8_t)(((px >> 11) & 0x1F) << 3);/* R */
        }
        fwrite(row, 1, row_bytes, f);
    }
    free(row);
    fclose(f);
    free(fb);
    ESP_LOGI(TAG, "screenshot -> %s", path);
    return ESP_OK;
}

void display_toggle_scale(void)
{
    s_scale = (s_scale == 1) ? 2 : 1;
    SDL_SetWindowSize(s_window, DISPLAY_WIDTH * s_scale, DISPLAY_HEIGHT * s_scale);
    SDL_SetWindowPosition(s_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
}

void display_window_to_fb(int wx, int wy, uint16_t *fx, uint16_t *fy)
{
    /* RenderCopy stretches the texture over the whole window (NULL dst rect),
     * so the mapping is a straight rescale by the current window size. */
    int win_w = DISPLAY_WIDTH, win_h = DISPLAY_HEIGHT;
    SDL_GetWindowSize(s_window, &win_w, &win_h);
    if (win_w <= 0) win_w = DISPLAY_WIDTH;
    if (win_h <= 0) win_h = DISPLAY_HEIGHT;

    int x = wx * DISPLAY_WIDTH  / win_w;
    int y = wy * DISPLAY_HEIGHT / win_h;
    if (x < 0) x = 0; else if (x >= DISPLAY_WIDTH)  x = DISPLAY_WIDTH  - 1;
    if (y < 0) y = 0; else if (y >= DISPLAY_HEIGHT) y = DISPLAY_HEIGHT - 1;
    *fx = (uint16_t)x;
    *fy = (uint16_t)y;
}

#endif /* BUILD_SIMULATOR */
