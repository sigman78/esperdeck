/* Task-context prototype. See scan8.h for alignment and buffer contract.
 * a2 dst, a3 rows, a4 pairs, a5 masks, a6 ncols.
 */
    .section .iram1,"ax"
    .align 4
    .global scan8_s3
    .type scan8_s3,@function
scan8_s3:
    entry a1, 32
    beqz a6, .Ldone
    movi a12, 1600
.Lcell:
    ee.vldbc.32.ip q0, a4, 0
    addi a13, a4, 12
    ee.vldbc.32.ip q1, a13, 0
    ee.xorq q1, q0, q1
    mov a9, a2
    movi a8, 16
    loopnez a8, .Lrows_end
        l8ui a10, a3, 0
        slli a10, a10, 4
        add a11, a5, a10
        ee.vld.128.ip q2, a11, 0
        ee.andq q3, q1, q2
        ee.xorq q3, q0, q3
        ee.vst.128.ip q3, a9, 0
        addi a3, a3, 1
        add a9, a9, a12
.Lrows_end:
    addi a2, a2, 16
    addi a4, a4, 16
    addi a6, a6, -1
    bnez a6, .Lcell
.Ldone:
    retw.n
    .size scan8_s3, .-scan8_s3
