/*
 * render_fx_pass.c — effect APPLICATION on rendered bands: clip window,
 * wobble, static burst, bell tag, and the once-per-frame tick. Effect
 * STATE is owned by display_fx.c (display_fx_internal.h); all passes are
 * bounded per-band work — none is hot.
 */

#include "render_internal.h"
#include "display_fx_internal.h"

/* The renderer's per-frame view of g_fx_cfg (see render_internal.h). */
DRAM_ATTR display_fx_cfg_t g_fx_snap = {};   /* set_font seeds it pre-scan */

IRAM_ATTR void render_fx_snapshot(void)
{
    /* Seqlock read, no spinning: on an in-flight write keep last frame's
     * snapshot — the ISR preempts the writer on this core, so waiting
     * could never finish. */
    const uint8_t g1 = g_fx_cfg_gen;
    const display_fx_cfg_t tmp = g_fx_cfg;
    if ((g1 & 1) == 0 && g1 == g_fx_cfg_gen)
        g_fx_snap = tmp;
}

/* Visual bell: a red tag flashed in the top-right corner by the ISR. */
#define BELL_TOTAL   40    /* 4 half-periods → two on/off flashes, ~1.0 s */
#define BELL_HALF    10    /* frames per on (or off) half, ~256 ms @39 Hz */

static DRAM_ATTR struct {
    volatile int frames;   /* armed from any task via display_bell()  */
    bool         show;     /* ISR-only, latched at the frame tick     */
} s_bell = {};

void display_bell(void) { s_bell.frames = BELL_TOTAL; }

/* Tick per-frame counters — called once per frame (scanline 0). */
IRAM_ATTR void render_fx_frame_tick(void)
{
    render_fx_snapshot();
    g_fx_frame++;
    if (g_fx_wipe_left     > 0) g_fx_wipe_left--;
    if (g_fx_collapse_left > 0) g_fx_collapse_left--;
    if (g_fx_static_left   > 0) g_fx_static_left--;
    /* Visual bell: latch this frame's on/off flash phase. */
    if (s_bell.frames > 0) {
        s_bell.show = ((BELL_TOTAL - s_bell.frames) / BELL_HALF) % 2 == 0;
        s_bell.frames--;
    } else {
        s_bell.show = false;
    }
}

/* Wipe / collapse clip window: while active, only scanlines in [wv0, wv1)
 * show content; the rest go black, with bright edge lines. */
IRAM_ATTR void render_fx_clip_window(fx_clip_t *c)
{
    c->active = false;
    c->wv0 = 0;
    c->wv1 = DISPLAY_HEIGHT;
    c->ea0 = c->ea1 = c->eb0 = c->eb1 = 0;
    c->ecol = 0;

    if (g_fx_collapse_left > 0 && g_fx_collapse_total > 2) {
        c->active = true;
        c->ecol = 0xFFFFu;                   /* hot white collapsing edges */
        if (g_fx_collapse_left <= 2) {       /* last 2 frames: center flash */
            c->wv0 = c->wv1 = 0;
            c->ea0 = DISPLAY_HEIGHT / 2 - 1;
            c->ea1 = c->ea0 + 2;
        } else {
            /* Window shrinks to ~0 by the time the flash frames start. */
            const int half = (DISPLAY_HEIGHT / 2) * (g_fx_collapse_left - 2)
                                                  / (g_fx_collapse_total - 2);
            c->wv0 = DISPLAY_HEIGHT / 2 - half;
            c->wv1 = DISPLAY_HEIGHT / 2 + half;
            c->ea0 = c->wv0; c->ea1 = c->wv0 + 2;
            c->eb0 = c->wv1 - 2; c->eb1 = c->wv1;
        }
    } else if (g_fx_wipe_left > 0 && g_fx_wipe_total > 0) {
        c->active = true;
        c->ecol = 0x07FFu;                   /* cyan leading edge */
        const int reveal = DISPLAY_HEIGHT * (g_fx_wipe_total - g_fx_wipe_left)
                                          / g_fx_wipe_total;
        c->wv1 = reveal;
        c->ea0 = reveal;
        c->ea1 = reveal + 2 > DISPLAY_HEIGHT ? DISPLAY_HEIGHT : reveal + 2;
    }
}

IRAM_ATTR bool render_fx_band_hidden(const fx_clip_t *c, int band_y0,
                                     int num_scans)
{
    return c->active && (band_y0 >= c->wv1 || band_y0 + num_scans <= c->wv0);
}

/* Fully hidden band: black, plus any edge lines that fall inside it. */
IRAM_ATTR void render_fx_fill_hidden(color_t *dst, int band_y0, int num_scans,
                                     const fx_clip_t *c)
{
    uint32_t *p = (uint32_t *)dst;
    const int words = num_scans * (DISPLAY_WIDTH / 2);
    for (int i = 0; i < words; i++) p[i] = 0;

    const uint32_t e2 = (uint32_t)c->ecol | ((uint32_t)c->ecol << 16);
    for (int n = 0; n < num_scans; n++) {
        const int y = band_y0 + n;
        if ((y >= c->ea0 && y < c->ea1) || (y >= c->eb0 && y < c->eb1)) {
            uint32_t *e = (uint32_t *)(dst + (unsigned)n * DISPLAY_WIDTH);
            for (int i = 0; i < DISPLAY_WIDTH / 2; i++) e[i] = e2;
        }
    }
}

/* Post-pass for bands straddling the visible window: black out hidden
 * scanlines, then draw the bright edge lines. */
IRAM_ATTR void render_fx_clip_apply(color_t *dst, int band_y0, int num_scans,
                                    const fx_clip_t *c)
{
    const uint32_t e2 = (uint32_t)c->ecol | ((uint32_t)c->ecol << 16);
    for (int n = 0; n < num_scans; n++) {
        const int y = band_y0 + n;
        uint32_t *p = (uint32_t *)(dst + (unsigned)n * DISPLAY_WIDTH);
        if ((y >= c->ea0 && y < c->ea1) || (y >= c->eb0 && y < c->eb1)) {
            for (int i = 0; i < DISPLAY_WIDTH / 2; i++) p[i] = e2;
        } else if (y < c->wv0 || y >= c->wv1) {
            for (int i = 0; i < DISPLAY_WIDTH / 2; i++) p[i] = 0;
        }
    }
}

/*
 * CRT line wobble — a ~16-scanline S-wiggle sweeping down the screen. The
 * displacement is handed to the scan as a per-scanline destination WORD
 * offset (pixels land wobbled as written; no shift pass), which is why it is
 * quantised to EVEN pixels: RGB565 packs two pixels per 32-bit word, and an
 * odd displacement would be a half-word offset the scan cannot express.
 * Design + measured numbers: docs/performance.md § "Wobble fix".
 *
 * Fills @p out with one word offset per scanline of the band and returns true
 * if any is non-zero (false lets the scan take its untouched fast path).
 */
IRAM_ATTR bool render_fx_wobble_offsets(int start_scan, int num_scans, int8_t *out)
{
    for (int i = 0; i < num_scans; i++)
        out[i] = 0;

    if (!g_fx_snap.wobble)
        return false;

    /* wob_env = sin(2*pi*(k+1)/17)*127: one full vertical period. dxa adds
     * ±25% amplitude drift (~10 Hz LCG); yc walks the wiggle top down the
     * screen over the 256-frame cycle (>479 = resting). */
    static DRAM_ATTR const int8_t wob_env[16] = {
         46,  86, 114, 126, 122, 101,  67,  23,
        -23, -67, -101, -122, -126, -114, -86, -46,
    };
    const int dx = g_fx_wobble_lut[g_fx_frame];
    const uint32_t hf = (uint32_t)(g_fx_frame >> 2) * 2246822519u;
    const int dxa = (dx * (12 + (int)((hf >> 7) & 7))) >> 4;
    const int yc = (int)(((unsigned)g_fx_frame * 5u) >> 1);
    const int n0 = yc - start_scan;
    bool any = false;
    for (int k = 0; k < 16 && dx != 0; k++) {
        const int n = n0 + k;
        if (n < 0 || n >= num_scans) continue;     /* other band / off */
        /* ±2 px jitter hashed from absolute y + frame — sub-row bands of
         * one frame agree at their seam. Even, like the envelope term. */
        const uint32_t hr = ((uint32_t)(start_scan + n) * 2654435761u)
                          ^ ((uint32_t)(g_fx_frame >> 1) * 2246822519u);
        int d = (dxa * (int)wob_env[k] + 64) >> 7;
        d = (d / 2) * 2;                           /* truncate toward zero */
        d += ((int)((hr >> 9) % 3u) - 1) * 2;
        if (d <= -DISPLAY_WIDTH || d >= DISPLAY_WIDTH) continue;
        out[n] = (int8_t)(d / 2);                  /* pixels -> words */
        any |= out[n] != 0;
    }
    return any;
}

/* Signal-loss static burst — grayscale snow on a few pseudo-random
 * scanlines; transient by design (the one per-pixel-ish pass allowed).
 * The table is 16 packed pixel-pairs indexed by a cheap LCG. */
static DRAM_ATTR const uint32_t s_fx_noise[16] = {
    0x0000FFFFu, 0xFFFF0000u, 0x00000000u, 0xFFFFFFFFu,
    0x84108410u, 0x00008410u, 0x84100000u, 0xC618C618u,
    0x42084208u, 0x0000C618u, 0xFFFF8410u, 0x21042104u,
    0x4208FFFFu, 0x00002104u, 0xC6184208u, 0x8410FFFFu,
};

IRAM_ATTR void render_fx_static(color_t *dst, int start_scan, int num_scans)
{
    if (g_fx_static_left <= 0 || num_scans <= 0)
        return;

    int nl = g_fx_snap.static_lines;
    if (nl > 4) nl = 4;
    /* Seed by BAND, not char row — a per-row seed would repeat the same
     * snow in both halves of a tall row, reading as structure. */
    uint32_t h = ((uint32_t)(start_scan / g_rs.band) * 2654435761u)
               ^ ((uint32_t)g_fx_frame * 2246822519u);
    for (int k = 0; k < nl; k++) {
        h = h * 1664525u + 1013904223u;
        /* Unbiased 0..num_scans-1 from 4 hash bits (scale, not clamp). */
        int n = (int)((((h >> 27) & 15u) * (unsigned)num_scans) >> 4);
        uint32_t *p = (uint32_t *)(dst + (unsigned)n * DISPLAY_WIDTH);
        uint32_t r = h;
        for (int i = 0; i < DISPLAY_WIDTH / 2; i += 4) {
            r = r * 1664525u + 1013904223u;
            p[i]     = s_fx_noise[r         & 15];
            p[i + 1] = s_fx_noise[(r >> 4)  & 15];
            p[i + 2] = s_fx_noise[(r >> 8)  & 15];
            p[i + 3] = s_fx_noise[(r >> 12) & 15];
        }
    }
}

/* Bell tag — a 16x16 white bell on a red 32px tag, top-right corner; each
 * band of row 0 draws its slice (glyph_row0 offsets into the bitmap). */
IRAM_ATTR void render_fx_bell_tag(color_t *dst, int char_row, int glyph_row0,
                                  int num_scans)
{
    if (char_row != 0 || !s_bell.show || glyph_row0 >= 16)
        return;

    static DRAM_ATTR const uint16_t bell[16] = {
        0x0180, 0x0180, 0x03C0, 0x07E0, 0x07E0, 0x0FF0, 0x0FF0, 0x1FF8,
        0x1FF8, 0x3FFC, 0x3FFC, 0x7FFE, 0x7FFE, 0x0000, 0x0180, 0x0180,
    };
    const color_t red   = 0xF800u;
    const color_t white = 0xFFFFu;
    const int tag_w  = 32;                          /* 32 px wide            */
    const int x0     = DISPLAY_WIDTH - tag_w;       /* hard top-right corner */
    const int bell_x = x0 + (tag_w - 16) / 2;       /* centre the 16px bell  */

    int nrows = num_scans;
    if (nrows > 16 - glyph_row0) nrows = 16 - glyph_row0;   /* 16 px tall */
    for (int n = 0; n < nrows; n++) {
        color_t *p = dst + n * DISPLAY_WIDTH + x0;
        for (int i = 0; i < tag_w; i++) p[i] = red;         /* red background */
        uint16_t bits = bell[glyph_row0 + n];
        color_t *b = dst + n * DISPLAY_WIDTH + bell_x;
        for (int col = 0; col < 16; col++)
            if ((bits >> (15 - col)) & 1u) b[col] = white;  /* white bell     */
    }
}
