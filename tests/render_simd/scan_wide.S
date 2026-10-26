#include "simd_guard.inc"
    .section .iram1,"ax"

/* a2 dst, a3 rows, a4 even pairs, a5 odd pairs, a6 masks, a7 row0.
 * Fixed 800-pixel output stride, half-row bands, full grid width.
 * Each cell reuses both palettes across its entire band. */
    .macro EMIT_ROW width, parity, store, rowoffset
    l16ui a10, a3, \rowoffset
    extui a11, a10, (\width - 8), 8
    slli a11, a11, 4
    add a11, a6, a11
    ee.vld.128.ip q4, a11, 0
    .if \parity == 0
    ee.andq q5, q1, q4
    ee.xorq q5, q0, q5
    .else
    ee.andq q5, q3, q4
    ee.xorq q5, q2, q5
    .endif
    .if \store == 0
    ee.vst.128.ip q5, a9, 0
    .elseif \store == 1
    ee.vst.l.64.ip q5, a9, 8
    ee.vst.h.64.ip q5, a9, -8
    .else
    ee.movi.32.a q5, a11, 0
    s32i a11, a9, 0
    ee.movi.32.a q5, a11, 1
    s32i a11, a9, 4
    ee.movi.32.a q5, a11, 2
    s32i a11, a9, 8
    ee.movi.32.a q5, a11, 3
    s32i a11, a9, 12
    .endif
    extui a11, a10, (\width - 10), 2
    .if \parity == 0
    addx4 a11, a11, a4
    .else
    addx4 a11, a11, a5
    .endif
    l32i a11, a11, 0
    s32i a11, a9, 16
    .if \width == 12
    extui a11, a10, 0, 2
    .if \parity == 0
    addx4 a11, a11, a4
    .else
    addx4 a11, a11, a5
    .endif
    l32i a11, a11, 0
    s32i a11, a9, 20
    .endif
    add a9, a9, a12
    .endm

    .macro WIDE_KERNEL name, width, cols
    .align 4
    .global \name
    .type \name,@function
\name:
    SAVE_SIMD
    addx2 a3, a7, a3
    movi a7, \cols
    movi a12, 1600
.Lcell\@:
    ee.vldbc.32.ip q0, a4, 0
    addi a13, a4, 12
    ee.vldbc.32.ip q1, a13, 0
    ee.xorq q1, q0, q1
    ee.vldbc.32.ip q2, a5, 0
    addi a13, a5, 12
    ee.vldbc.32.ip q3, a13, 0
    ee.xorq q3, q2, q3
    mov a9, a2
    extui a8, a2, 0, 4
    beqz a8, .Laligned\@
    extui a8, a2, 0, 3
    beqz a8, .Lhalf\@
    movi a8, (\width / 2)
    loopnez a8, .Lword_end\@
    EMIT_ROW \width, 0, 2, 0
    EMIT_ROW \width, 1, 2, 2
    addi a3, a3, 4
.Lword_end\@:
    j .Lnext\@
.Lhalf\@:
    movi a8, (\width / 2)
    loopnez a8, .Lhalf_end\@
    EMIT_ROW \width, 0, 1, 0
    EMIT_ROW \width, 1, 1, 2
    addi a3, a3, 4
.Lhalf_end\@:
    j .Lnext\@
.Laligned\@:
    movi a8, (\width / 2)
    loopnez a8, .Laligned_end\@
    EMIT_ROW \width, 0, 0, 0
    EMIT_ROW \width, 1, 0, 2
    addi a3, a3, 4
.Laligned_end\@:
.Lnext\@:
    addi a3, a3, (\width * 2)
    addi a2, a2, (\width * 2)
    addi a4, a4, 16
    addi a5, a5, 16
    addi a7, a7, -1
    bnez a7, .Lcell\@
    RESTORE_SIMD
    .size \name, .-\name
    .endm

    WIDE_KERNEL scan10_guarded, 10, 80
    WIDE_KERNEL scan12_guarded, 12, 66
