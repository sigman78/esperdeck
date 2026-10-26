/* Benchmark kernels touch q0..q5 and SAR_BYTE only. Accumulators/q6/q7
 * remain untouched. CPENABLE is restored even when CP3 was initially off. */
    .macro SAVE_SIMD
    entry a1, 144
    rsr a14, CPENABLE
    s32i a14, a1, 16
    movi a15, 8
    or a15, a14, a15
    wsr a15, CPENABLE
    rsync
    rur.sar_byte a14
    s32i a14, a1, 20
    addi a8, a1, 32
    ee.vst.128.ip q0, a8, 16
    ee.vst.128.ip q1, a8, 16
    ee.vst.128.ip q2, a8, 16
    ee.vst.128.ip q3, a8, 16
    ee.vst.128.ip q4, a8, 16
    ee.vst.128.ip q5, a8, 16
    .endm

    .macro RESTORE_SIMD
    addi a8, a1, 32
    ee.vld.128.ip q0, a8, 16
    ee.vld.128.ip q1, a8, 16
    ee.vld.128.ip q2, a8, 16
    ee.vld.128.ip q3, a8, 16
    ee.vld.128.ip q4, a8, 16
    ee.vld.128.ip q5, a8, 16
    l32i a14, a1, 20
    wur.sar_byte a14
    l32i a14, a1, 16
    wsr a14, CPENABLE
    rsync
    retw.n
    .endm
