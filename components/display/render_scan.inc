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
 *
 * cx->xoff carries the CRT wobble as a per-scanline destination word offset,
 * so displaced lines land wobbled as they are written instead of being
 * shifted by a second pass. It is NULL whenever no line in the band moves,
 * which is the overwhelming majority: the wiggle spans 16 scanlines, so at
 * 8x16 it touches one band in thirty. That case gets its own copy of the
 * loop below rather than a per-scanline `koff` — folding the offset into one
 * shared loop measured +5.9% on the undisplaced path, because the extra live
 * values disturb register allocation in a loop tuned down to the cycle.
 */

static IRAM_ATTR void SCAN_FN(const scan_ctx_t *cx)
{
    enum { W = SCAN_W, CW = SCAN_W / 2,    /* CW: uint32 stores per column */
           NW = DISPLAY_WIDTH / 2 };       /* destination words per scanline */
    const int ncols = cx->ncols;
    const uint8_t *const rows = cx->rows;

    if (!cx->xoff) {
        /* ---- undisplaced band: the original loop, untouched ---- */
        int hi = ncols * CW;               /* black margin right of the grid */
        if (hi > NW) hi = NW;
        for (int n = 0; n < cx->num_scans; n++) {
            const int gl = cx->glyph_row0 + n;
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
            if (c < ncols) {
                const SCAN_ROW *g0 = (const SCAN_ROW *)(rows
                        + (size_t)c * (2u * W * sizeof(SCAN_ROW)));
                const unsigned b0 = g0[gl];
                RENDER_UNROLL
                for (int p = 0; p < W; p += 2)
                    d[p >> 1] = scan_gpair(b0, W, p, bgv[c], xfv[c]);
                d += CW;
            }
            for (int i = hi; i < NW; i++)
                ((uint32_t *)(cx->dst + (unsigned)n * DISPLAY_WIDTH))[i] = 0;
        }
        return;
    }

    /* ---- displaced band: same scan, written at a per-scanline offset ---- */
    for (int n = 0; n < cx->num_scans; n++) {
        const int gl = cx->glyph_row0 + n;
        const int sel = n & cx->scan_on & 1;
        const uint16_t *bgv = cx->bg[sel];
        const uint16_t *xfv = cx->xf[sel];
        uint32_t *const line = (uint32_t *)(cx->dst + (unsigned)n * DISPLAY_WIDTH);

        /* Cell c lands on destination words [koff + c*CW, koff + (c+1)*CW).
         * [c0, c1) is the fully-visible range; at most one cell at each end
         * is clipped, because |koff| never exceeds CW. */
        const int koff = cx->xoff[n];
        int c0 = 0, c1 = ncols;
        if (koff > 0) {
            c1 = (NW - koff) / CW;
            if (c1 > ncols) c1 = ncols;
        } else if (koff < 0) {
            c0 = (-koff + CW - 1) / CW;
            if (c0 > ncols) c0 = ncols;
        }

        uint32_t *d = line + koff + c0 * CW;

        int c = c0;
        for (; c + 1 < c1; c += 2) {
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
        if (c < c1) {
            const SCAN_ROW *g0 = (const SCAN_ROW *)(rows
                    + (size_t)c * (2u * W * sizeof(SCAN_ROW)));
            const unsigned b0 = g0[gl];
            RENDER_UNROLL
            for (int p = 0; p < W; p += 2)
                d[p >> 1] = scan_gpair(b0, W, p, bgv[c], xfv[c]);
        }

        {
            /* The one clipped cell at whichever edge the shift vacated. */
            if (koff > 0 && c1 < ncols) {
                const int base = koff + c1 * CW;
                const int nw   = NW - base;          /* 0 .. CW-1 */
                const SCAN_ROW *g = (const SCAN_ROW *)(rows
                        + (size_t)c1 * (2u * W * sizeof(SCAN_ROW)));
                const unsigned b  = g[gl];
                const uint16_t bg = bgv[c1], xf = xfv[c1];
                for (int w = 0; w < nw; w++)
                    line[base + w] = scan_gpair(b, W, w * 2, bg, xf);
            } else if (koff < 0 && c0 > 0) {
                const int cp   = c0 - 1;
                const int base = koff + cp * CW;      /* negative */
                const SCAN_ROW *g = (const SCAN_ROW *)(rows
                        + (size_t)cp * (2u * W * sizeof(SCAN_ROW)));
                const unsigned b  = g[gl];
                const uint16_t bg = bgv[cp], xf = xfv[cp];
                for (int w = -base; w < CW; w++)      /* first visible word */
                    line[base + w] = scan_gpair(b, W, w * 2, bg, xf);
            }
        }

        /* Black everything the grid does not cover: the edge the shift
         * vacated, and the margin right of the text area (12x24 only). */
        const int lo = koff > 0 ? koff : 0;
        int hi = koff + ncols * CW;
        if (hi > NW) hi = NW;
        for (int i = 0; i < lo; i++)  line[i] = 0;
        for (int i = hi; i < NW; i++) line[i] = 0;
    }
}
