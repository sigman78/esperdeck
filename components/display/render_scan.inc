/*
 * render_scan.inc — the HOT band-scan loop, "templatized" the C way:
 * #include once per font size (see render_scan.c) after defining
 *
 *   SCAN_W    cell width in pixels (integer literal: 8, 10, 12)
 *   SCAN_ROW  glyph row type: uint8_t (W <= 8) or uint16_t
 *   SCAN_FN   name of the emitted function
 *
 * This loop IS the ISR's render floor (~13 cycles/pixel) — treat any edit
 * as hot-path work and re-run the CYBERDECK_BENCH_STRESS numbers. Two
 * load-bearing choices at -Os: columns go in PAIRS with hoisted loads,
 * and the pixel loops need RENDER_UNROLL. The glyph stride folds to the
 * constant 2*W*sizeof(SCAN_ROW) because cell height == 2*W for all sizes.
 */

static IRAM_ATTR void SCAN_FN(const scan_ctx_t *cx)
{
    enum { W = SCAN_W, CW = SCAN_W / 2 };   /* CW: uint32 stores per column */
    const int ncols = cx->ncols;
    const uint8_t *const rows = cx->rows;

    for (int n = 0; n < cx->num_scans; n++) {
        const int gl = cx->glyph_row0 + n;
        /* Odd scanlines read the pre-dimmed variant; bands start on even
         * scanlines, so n's parity is global parity. */
        const int sel = n & cx->scan_on & 1;
        const uint16_t *bgv = cx->bg[sel];
        const uint16_t *xfv = cx->xf[sel];
        uint32_t *d = (uint32_t *)(cx->dst + (unsigned)n * DISPLAY_WIDTH);

        int c = 0;
        for (; c + 1 < ncols; c += 2) {
            const SCAN_ROW *g0 = (const SCAN_ROW *)(rows
                    + (size_t)(c    ) * (2u * W * sizeof(SCAN_ROW)));
            const SCAN_ROW *g1 = (const SCAN_ROW *)(rows
                    + (size_t)(c + 1) * (2u * W * sizeof(SCAN_ROW)));
            const unsigned b0  = g0[gl];
            const unsigned b1  = g1[gl];
            const uint16_t bg0 = bgv[c    ], xf0 = xfv[c    ];
            const uint16_t bg1 = bgv[c + 1], xf1 = xfv[c + 1];

            RENDER_UNROLL
            for (int p = 0; p < W; p += 2)
                d[p >> 1] = scan_gpair(b0, W, p, bg0, xf0);
            RENDER_UNROLL
            for (int p = 0; p < W; p += 2)
                d[CW + (p >> 1)] = scan_gpair(b1, W, p, bg1, xf1);
            d += 2 * CW;
        }

        /* Trailing odd column (defensive; all shipped grids are even). */
        if (c < ncols) {
            const SCAN_ROW *g0 = (const SCAN_ROW *)(rows
                    + (size_t)c * (2u * W * sizeof(SCAN_ROW)));
            const unsigned b0 = g0[gl];
            RENDER_UNROLL
            for (int p = 0; p < W; p += 2)
                d[p >> 1] = scan_gpair(b0, W, p, bgv[c], xfv[c]);
            d += CW;
        }

        /* Blank the margin right of the text area (black). */
        for (int i = 0; i < cx->margin_words; i++)
            d[i] = 0;
    }
}
