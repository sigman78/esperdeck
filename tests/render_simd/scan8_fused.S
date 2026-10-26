#include "simd_guard.inc"
    .section .iram1,"ax"

/* a2 output, a3 selected row in glyph 0, a4 selected palette, a5 masks.
 * q4 is the previous glyph; q5 is freshly expanded. No output rereads. */
    .macro EXPAND
    ee.vldbc.32.ip q0, a4, 0
    addi a11, a4, 12
    ee.vldbc.32.ip q1, a11, 0
    ee.xorq q1, q0, q1
    l8ui a9, a3, 0
    slli a9, a9, 4
    add a10, a5, a9
    ee.vld.128.ip q2, a10, 0
    ee.andq q5, q1, q2
    ee.xorq q5, q0, q5
    addi a3, a3, 16
    addi a4, a4, 16
    .endm

    .macro FUSED name, words
    .align 4
    .global \name
    .type \name,@function
\name:
    SAVE_SIMD
    .if \words <= 0
      .set whole, ((-\words) / 4)
      .set rem, ((-\words) % 4) * 4
      addi a3, a3, (whole * 16)
      addi a4, a4, (whole * 16)
      .if rem == 0
        movi a7, (100 - whole)
        loopnez a7, .Lfused_left_copy\@
        EXPAND
        ee.vst.128.ip q5, a2, 16
.Lfused_left_copy\@:
      .else
        movi a7, rem
        wur.sar_byte a7
        EXPAND
        mv.qr q4, q5
        movi a7, (99 - whole)
        loopnez a7, .Lfused_left\@
        EXPAND
        ee.src.q q3, q4, q5
        ee.vst.128.ip q3, a2, 16
        mv.qr q4, q5
.Lfused_left\@:
        ee.zero.q q5
        ee.src.q q3, q4, q5
        ee.vst.128.ip q3, a2, 16
      .endif
      .if whole > 0
        ee.zero.q q5
        ee.vst.128.ip q5, a2, 16
      .endif
    .else
      .set whole, (\words / 4)
      .set rem, (\words % 4) * 4
      ee.zero.q q4
      .if whole > 0
        ee.vst.128.ip q4, a2, 16
      .endif
      .if rem == 0
        movi a7, (100 - whole)
        loopnez a7, .Lfused_right_copy\@
        EXPAND
        ee.vst.128.ip q5, a2, 16
.Lfused_right_copy\@:
      .else
        movi a7, (16 - rem)
        wur.sar_byte a7
        movi a7, (100 - whole)
        loopnez a7, .Lfused_right\@
        EXPAND
        ee.src.q q3, q4, q5
        ee.vst.128.ip q3, a2, 16
        mv.qr q4, q5
.Lfused_right\@:
      .endif
    .endif
    RESTORE_SIMD
    .size \name, .-\name
    .endm

    FUSED fused_m5, -5
    FUSED fused_m4, -4
    FUSED fused_m3, -3
    FUSED fused_m2, -2
    FUSED fused_m1, -1
    FUSED fused_0, 0
    FUSED fused_p1, 1
    FUSED fused_p2, 2
    FUSED fused_p3, 3
    FUSED fused_p4, 4
