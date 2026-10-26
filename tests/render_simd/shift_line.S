#include "simd_guard.inc"
    .section .iram1,"ax"

/* Fixed-offset in-place memmove with black fill. Right shifts walk
 * backwards, left shifts forwards, so unread pixels are never overwritten.
 * Every vector access stays inside the 1600-byte scanline. */
    .macro SHIFT_LINE name, words
    .align 4
    .global \name
    .type \name,@function
\name:
    SAVE_SIMD
    .if \words < 0
      .set whole, ((-\words) / 4)
      .set rem, ((-\words) % 4) * 4
      addi a3, a2, (whole * 16)
      .if rem == 0
        movi a4, (100 - whole)
        loopnez a4, .Lleft_copy_end\@
        ee.vld.128.ip q0, a3, 16
        ee.vst.128.ip q0, a2, 16
.Lleft_copy_end\@:
      .else
        movi a4, rem
        wur.sar_byte a4
        ee.vld.128.ip q0, a3, 16
        movi a4, (99 - whole)
        loopnez a4, .Lleft_end\@
        ee.vld.128.ip q1, a3, 16
        ee.src.q q2, q0, q1
        ee.vst.128.ip q2, a2, 16
        mv.qr q0, q1
.Lleft_end\@:
        ee.zero.q q1
        ee.src.q q2, q0, q1
        ee.vst.128.ip q2, a2, 16
      .endif
      .if whole > 0
        ee.zero.q q0
        .rept whole
        ee.vst.128.ip q0, a2, 16
        .endr
      .endif
    .else
      .set whole, (\words / 4)
      .set rem, (\words % 4) * 4
      movi a3, ((99 - whole) * 16)
      add a3, a2, a3
      movi a4, 1584
      add a2, a2, a4
      .if rem == 0
        movi a4, (100 - whole)
        loopnez a4, .Lright_copy_end\@
        ee.vld.128.ip q0, a3, -16
        ee.vst.128.ip q0, a2, -16
.Lright_copy_end\@:
      .else
        movi a4, (16 - rem)
        wur.sar_byte a4
        ee.vld.128.ip q1, a3, -16
        movi a4, (99 - whole)
        loopnez a4, .Lright_end\@
        ee.vld.128.ip q0, a3, -16
        ee.src.q q2, q0, q1
        ee.vst.128.ip q2, a2, -16
        mv.qr q1, q0
.Lright_end\@:
        ee.zero.q q0
        ee.src.q q2, q0, q1
        ee.vst.128.ip q2, a2, -16
      .endif
      .if whole > 0
        ee.zero.q q0
        .rept whole
        ee.vst.128.ip q0, a2, -16
        .endr
      .endif
    .endif
    RESTORE_SIMD
    .size \name, .-\name
    .endm

    SHIFT_LINE shift_m5, -5
    SHIFT_LINE shift_m4, -4
    SHIFT_LINE shift_m3, -3
    SHIFT_LINE shift_m2, -2
    SHIFT_LINE shift_m1, -1
    SHIFT_LINE shift_p1, 1
    SHIFT_LINE shift_p2, 2
    SHIFT_LINE shift_p3, 3
    SHIFT_LINE shift_p4, 4
