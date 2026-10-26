/* Benchmark-only ISR experiment. Saves every vector register touched here,
 * restores CPENABLE, and touches no accumulator or SAR_BYTE register.
 * Stack is internal SRAM and 16-byte aligned under the windowed ABI.
 * a2 dst, a3 glyph rows, a4 even pairs, a5 odd pairs, a6 masks, a7 cols.
 */
    .section .iram1,"ax"
    .align 4
    .global scan8_guarded
    .type scan8_guarded,@function
scan8_guarded:
    entry a1, 144
    beqz a7, .Lguard_done
    rsr a14, CPENABLE
    s32i a14, a1, 16
    movi a15, 8
    or a15, a14, a15
    wsr a15, CPENABLE
    rsync
    addi a8, a1, 32
    ee.vst.128.ip q0, a8, 16
    ee.vst.128.ip q1, a8, 16
    ee.vst.128.ip q2, a8, 16
    ee.vst.128.ip q3, a8, 16
    ee.vst.128.ip q4, a8, 16
    ee.vst.128.ip q5, a8, 16
    movi a12, 1600
.Lguard_cell:
    ee.vldbc.32.ip q0, a4, 0
    addi a13, a4, 12
    ee.vldbc.32.ip q1, a13, 0
    ee.xorq q1, q0, q1
    ee.vldbc.32.ip q2, a5, 0
    addi a13, a5, 12
    ee.vldbc.32.ip q3, a13, 0
    ee.xorq q3, q2, q3
    mov a9, a2
    movi a8, 8
    loopnez a8, .Lguard_rows_end
        l8ui a10, a3, 0
        slli a10, a10, 4
        add a11, a6, a10
        ee.vld.128.ip q4, a11, 0
        ee.andq q5, q1, q4
        ee.xorq q5, q0, q5
        ee.vst.128.ip q5, a9, 0
        add a9, a9, a12
        l8ui a10, a3, 1
        slli a10, a10, 4
        add a11, a6, a10
        ee.vld.128.ip q4, a11, 0
        ee.andq q5, q3, q4
        ee.xorq q5, q2, q5
        ee.vst.128.ip q5, a9, 0
        addi a3, a3, 2
        add a9, a9, a12
.Lguard_rows_end:
    addi a2, a2, 16
    addi a4, a4, 16
    addi a5, a5, 16
    addi a7, a7, -1
    bnez a7, .Lguard_cell
    addi a8, a1, 32
    ee.vld.128.ip q0, a8, 16
    ee.vld.128.ip q1, a8, 16
    ee.vld.128.ip q2, a8, 16
    ee.vld.128.ip q3, a8, 16
    ee.vld.128.ip q4, a8, 16
    ee.vld.128.ip q5, a8, 16
    l32i a14, a1, 16
    wsr a14, CPENABLE
    rsync
.Lguard_done:
    retw.n
    .size scan8_guarded, .-scan8_guarded

/* Task-side test: seed q0..q5, call the guard, check all 96 bytes and
 * CPENABLE afterwards. Not used by the renderer or included in timings. */
    .align 4
    .global scan8_guard_probe
    .type scan8_guard_probe,@function
scan8_guard_probe:
    entry a1, 256
    mov a8, a6
    ee.vld.128.ip q0, a8, 16
    ee.vld.128.ip q1, a8, 16
    ee.vld.128.ip q2, a8, 16
    ee.vld.128.ip q3, a8, 16
    ee.vld.128.ip q4, a8, 16
    ee.vld.128.ip q5, a8, 16
    rsr a9, CPENABLE
    s32i a9, a1, 16
    rur.sar_byte a9
    s32i a9, a1, 20
    movi a9, 13
    wur.sar_byte a9
    addi a8, a1, 32
    ee.vst.128.ip q0, a8, 16
    ee.vst.128.ip q1, a8, 16
    ee.vst.128.ip q2, a8, 16
    ee.vst.128.ip q3, a8, 16
    ee.vst.128.ip q4, a8, 16
    ee.vst.128.ip q5, a8, 16
    /* In the live benchmark this lets LCD interrupts preempt a task with
     * nontrivial vectors, testing preservation across actual ISR entries. */
    movi a9, 1000000
.Lprobe_wait:
    addi a9, a9, -1
    bnez a9, .Lprobe_wait
    mov a10, a2
    mov a11, a3
    mov a12, a4
    mov a13, a5
    mov a14, a6
    mov a15, a7
    call8 scan8_guarded
    rsr a13, CPENABLE
    l32i a14, a1, 16
    bne a13, a14, .Lprobe_fail
    rur.sar_byte a13
    movi a14, 13
    bne a13, a14, .Lprobe_fail
    movi a9, 128
    add a9, a1, a9
    mov a8, a9
    ee.vst.128.ip q0, a8, 16
    ee.vst.128.ip q1, a8, 16
    ee.vst.128.ip q2, a8, 16
    ee.vst.128.ip q3, a8, 16
    ee.vst.128.ip q4, a8, 16
    ee.vst.128.ip q5, a8, 16
    addi a8, a1, 32
    movi a12, 24
.Lprobe_word:
    l32i a10, a8, 0
    l32i a11, a9, 0
    bne a10, a11, .Lprobe_fail
    addi a8, a8, 4
    addi a9, a9, 4
    addi a12, a12, -1
    bnez a12, .Lprobe_word
    movi a2, 0
    l32i a9, a1, 20
    wur.sar_byte a9
    retw.n
.Lprobe_fail:
    movi a2, 1
    l32i a9, a1, 20
    wur.sar_byte a9
    retw.n
    .size scan8_guard_probe, .-scan8_guard_probe
