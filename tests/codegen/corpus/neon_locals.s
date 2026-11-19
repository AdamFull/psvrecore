@ Synthetic corpus: hand-written code for the Advanced SIMD registers in locals (ARCHITECTURE.md, "Advanced SIMD"):
@ one Q register written and read as Q, D and S registers in one function (and VFP arithmetic on its lanes); NEON
@ values live across a direct call to a callee that touches no floating-point register, one that changes q4-q7
@ without saving them (outside AAPCS, where d8-d15 are callee-saved), one that saves q4 with vpush/vpop, and across an
@ indirect call; VLD1-4 and VST1-4 of every form (whole registers, register lists with spacing 2, single lanes, all
@ lanes) with and without writeback, by an immediate and by a register, at any alignment and ending exactly at the end
@ of the corpus' memory (a page that faults follows it in the production configuration); VFP and NEON on the same
@ registers in a loop; and loops as games write them: a 4x4 matrix product, vec4 normalisation with vrsqrte and
@ Newton steps, a saturating audio mix, a colour conversion with widening multiplies and narrowing shifts, a
@ vld1/vst1 copy. Every register is random at the start (special values included), as is the FPSCR.

    .syntax unified
    .fpu neon-fp16
    .text
    .thumb

@ ---- callees ----

@ No floating-point register touched.
    .global nl_leaf_int
    .type nl_leaf_int, %function
    .thumb_func
nl_leaf_int:
    add     r0, r0, #1
    bx      lr

@ Writes q0 and d2 (a NEON leaf).
    .global nl_neon_leaf
    .type nl_neon_leaf, %function
    .thumb_func
nl_neon_leaf:
    vadd.f32 q0, q0, q1
    veor    d2, d2, d3
    bx      lr

@ Changes q4-q7 without saving them.
    .global nl_clobber_hi
    .type nl_clobber_hi, %function
    .thumb_func
nl_clobber_hi:
    vmov.i32 q4, #0x55
    vmov.i32 q5, #7
    vadd.i32 q6, q6, q0
    vmov.i8 q7, #0x80
    bx      lr

@ Saves q4 (d8, d9), changes it, puts it back; changes q0.
    .global nl_saves_q4
    .type nl_saves_q4, %function
    .thumb_func
nl_saves_q4:
    vpush   {d8, d9}
    vmov.f32 q4, #1.0
    vadd.f32 q0, q0, q4
    vpop    {d8, d9}
    bx      lr

@ ---- tests ----

@ q0 written whole, read as d1 and s3, s1 written, q0 read whole again, d0 written from core registers; VFP
@ arithmetic on lanes of q3 (as S and D registers), lane moves, permutes in place.
@ @test nl_alias r0=any
    .global nl_alias
    .type nl_alias, %function
    .thumb_func
nl_alias:
    vmov.f32 q0, #1.0
    vadd.f32 q0, q0, q1
    vmov    r0, s3
    vadd.f32 d4, d1, d1
    vmov.f32 s1, #2.0
    vadd.i32 q2, q0, q0
    vmov    d0, r0, r0
    vsub.f32 q3, q0, q2
    vmov.f32 s14, s3
    vmul.f32 s12, s12, s13
    vadd.f64 d10, d10, d6
    vext.8  q5, q3, q5, #3
    vmov.32 r1, d7[1]
    vmov.32 d11[0], r1
    vtrn.16 d8, d9
    vzip.8  q6, q7
    vswp    d12, d15
    vrev64.32 q7, q2
    vmov    r2, r3, d5
    bx      lr

@ NEON values live across direct calls and an indirect one; q4 across a callee that changes it (read back) and one
@ that saves it (kept).
@ @test nl_calls r0=any
    .global nl_calls
    .type nl_calls, %function
    .thumb_func
nl_calls:
    push    {r4, lr}
    vadd.f32 q2, q0, q1
    vorr    q8, q2, q2
    vadd.i32 q4, q4, q2
    bl      nl_leaf_int
    vadd.f32 q2, q2, q8
    bl      nl_clobber_hi
    vadd.i32 q9, q4, q5
    vadd.i32 q12, q6, q7
    bl      nl_saves_q4
    vadd.f32 q10, q0, q4
    movw    r4, #:lower16:nl_neon_leaf
    movt    r4, #:upper16:nl_neon_leaf
    blx     r4
    vadd.f32 q11, q0, q2
    vsub.f32 d26, d2, d16
    pop     {r4, pc}

@ Every load and store form, with writeback by the size and by a register, at any alignment.
@ @test nl_ldst r0=ptr r1=ptr r2=int:0:16
    .global nl_ldst
    .type nl_ldst, %function
    .thumb_func
nl_ldst:
    vld1.8  {d0}, [r0]!
    vld1.16 {d1, d2}, [r0], r2
    vld1.32 {d3, d4, d5}, [r0]!
    vld1.64 {d6, d7, d8, d9}, [r0]
    vld2.8  {d10, d11}, [r0]!
    vld2.16 {d12, d14}, [r0]
    vld2.32 {d16, d17, d18, d19}, [r0]!
    vld3.8  {d20, d21, d22}, [r0]!
    vld3.16 {d21, d23, d25}, [r0], r2
    vld4.8  {d24, d25, d26, d27}, [r0]!
    vld4.32 {d0, d2, d4, d6}, [r0]
    vld1.32 {d1[1]}, [r0]!
    vld2.16 {d3[2], d4[2]}, [r0]!
    vld3.8  {d5[7], d6[7], d7[7]}, [r0], r2
    vld4.32 {d8[0], d9[0], d10[0], d11[0]}, [r0]
    vld4.16 {d24[3], d26[3], d28[3], d30[3]}, [r0]!
    vld1.32 {d12[]}, [r0]
    vld1.16 {d13[], d14[]}, [r0]!
    vld2.8  {d15[], d16[]}, [r0]!
    vld3.16 {d17[], d18[], d19[]}, [r0]
    vld4.8  {d20[], d21[], d22[], d23[]}, [r0]!
    vld2.32 {d29[], d31[]}, [r0], r2
    vst1.8  {d0, d1, d2, d3}, [r1]!
    vst2.16 {d4, d5}, [r1], r2
    vst2.32 {d6, d8}, [r1]!
    vst2.8  {d8, d9, d10, d11}, [r1]!
    vst3.32 {d12, d13, d14}, [r1]!
    vst3.8  {d16, d18, d20}, [r1]
    vst4.16 {d20, d21, d22, d23}, [r1]!
    vst4.8  {d24, d26, d28, d30}, [r1], r2
    vst1.32 {d1[1]}, [r1]!
    vst2.8  {d3[5], d4[5]}, [r1]!
    vst3.16 {d5[1], d6[1], d7[1]}, [r1]!
    vst4.32 {d8[1], d9[1], d10[1], d11[1]}, [r1]
    vst1.64 {d12, d13}, [r1]!
    vst1.16 {d14, d15, d16}, [r1]
    bx      lr

@ Loads and stores that end exactly at the end of the corpus' memory, at odd addresses: nothing past it may be read or
@ written (the production configuration's page after it faults).
@ @test nl_tail r0=tail:24 r1=tail:41 r2=tail:4 r3=tail:16
    .global nl_tail
    .type nl_tail, %function
    .thumb_func
nl_tail:
    vld3.8  {d0, d1, d2}, [r0]
    vld1.8  {d4, d5}, [r1]!
    vld1.8  {d6, d7}, [r1]
    vld4.8  {d16[], d17[], d18[], d19[]}, [r2]
    vld1.32 {d20[1]}, [r2]
    vld2.32 {d22, d23}, [r3]
    vld1.8  {d24}, [r0]
    vadd.i8 q2, q2, q3
    vst3.8  {d4, d5, d6}, [r0]
    vst1.8  {d0, d1}, [r3]
    vst4.8  {d16[1], d17[1], d18[1], d19[1]}, [r2]
    vst1.16 {d20, d21}, [r3]
    bx      lr

@ VFP and NEON on the same registers in a loop (q0/q1 as S, D and Q registers).
@ @test nl_mixed r0=int:0:8
    .global nl_mixed
    .type nl_mixed, %function
    .thumb_func
nl_mixed:
    cbz     r0, 2f
1:  vmul.f32 q1, q1, q0
    vadd.f32 s4, s4, s0
    vcvt.f64.f32 d8, s5
    vadd.f64 d9, d9, d8
    vcvt.f32.f64 s6, d9
    vneg.f32 q2, q1
    vmla.f32 d0, d2, d4[1]
    vmov.f32 s3, s9
    subs    r0, r0, #1
    bne     1b
2:  bx      lr

@ out = a * b, 4x4 single-precision matrices (column vectors by lane).
@ @test nl_mat4x4 r0=fptr r1=fptr r2=fptr
    .global nl_mat4x4
    .type nl_mat4x4, %function
    .thumb_func
nl_mat4x4:
    vld1.32 {d16, d17, d18, d19}, [r1]!
    vld1.32 {d20, d21, d22, d23}, [r1]
    vld1.32 {d0, d1, d2, d3}, [r2]!
    vld1.32 {d4, d5, d6, d7}, [r2]
    vmul.f32 q12, q8, d0[0]
    vmla.f32 q12, q9, d0[1]
    vmla.f32 q12, q10, d1[0]
    vmla.f32 q12, q11, d1[1]
    vmul.f32 q13, q8, d2[0]
    vmla.f32 q13, q9, d2[1]
    vmla.f32 q13, q10, d3[0]
    vmla.f32 q13, q11, d3[1]
    vmul.f32 q14, q8, d4[0]
    vmla.f32 q14, q9, d4[1]
    vmla.f32 q14, q10, d5[0]
    vmla.f32 q14, q11, d5[1]
    vmul.f32 q15, q8, d6[0]
    vmla.f32 q15, q9, d6[1]
    vmla.f32 q15, q10, d7[0]
    vmls.f32 q15, q11, d7[1]
    vst1.32 {d24, d25, d26, d27}, [r0]!
    vst1.32 {d28, d29, d30, d31}, [r0]
    bx      lr

@ vec4s normalised in place: the squared length by pairwise adds, vrsqrte and two Newton steps (vrsqrts).
@ @test nl_normalize r0=fptr r1=int:0:16
    .global nl_normalize
    .type nl_normalize, %function
    .thumb_func
nl_normalize:
    cbz     r1, 2f
1:  vld1.32 {d0, d1}, [r0]
    vmul.f32 q1, q0, q0
    vpadd.f32 d2, d2, d3
    vpadd.f32 d2, d2, d2
    vrsqrte.f32 d3, d2
    vmul.f32 d4, d3, d2
    vrsqrts.f32 d4, d4, d3
    vmul.f32 d3, d3, d4
    vmul.f32 d4, d3, d2
    vrsqrts.f32 d4, d4, d3
    vmul.f32 d3, d3, d4
    vmul.f32 q0, q0, d3[0]
    vrecpe.f32 d5, d2
    vrecps.f32 d6, d5, d2
    vmul.f32 d5, d5, d6
    vst1.32 {d0, d1}, [r0]!
    subs    r1, r1, #1
    bne     1b
2:  bx      lr

@ An s16 mix: dst = sat(dst + sat(src * gain)), with a rounding doubling multiply and a long product narrowed back.
@ @test nl_mix r0=ptr:2 r1=ptr:2 r2=int:0:16 r3=any
    .global nl_mix
    .type nl_mix, %function
    .thumb_func
nl_mix:
    vdup.16 d7, r3
    cbz     r2, 2f
1:  vld1.16 {d0, d1}, [r0]
    vld1.16 {d2, d3}, [r1]!
    vqrdmulh.s16 q1, q1, d7[0]
    vqadd.s16 q0, q0, q1
    vmull.s16 q2, d0, d7
    vqshrn.s32 d4, q2, #15
    vqsub.s16 d1, d1, d4
    vst1.16 {d0, d1}, [r0]!
    subs    r2, r2, #1
    bne     1b
2:  vmrs    r0, fpscr
    bx      lr

@ RGBA8 to luma and back: widening multiplies by constants, rounding narrowing shifts, saturating narrowing.
@ @test nl_color r0=ptr r1=ptr r2=int:0:8
    .global nl_color
    .type nl_color, %function
    .thumb_func
nl_color:
    vmov.i8 d28, #77
    vmov.i8 d29, #150
    vmov.i8 d30, #29
    cbz     r2, 2f
1:  vld4.8  {d0, d1, d2, d3}, [r0]!
    vmull.u8 q8, d0, d28
    vmlal.u8 q8, d1, d29
    vmlal.u8 q8, d2, d30
    vrshrn.u16 d4, q8, #8
    vmovl.u8 q9, d3
    vshl.i16 q9, q9, #2
    vsub.i16 q9, q9, q8
    vqmovun.s16 d5, q9
    vshll.i8 q10, d4, #8
    vqrshrn.u16 d6, q10, #8
    vst3.8  {d4, d5, d6}, [r1]!
    subs    r2, r2, #1
    bne     1b
2:  bx      lr

@ A copy by 32-byte vld1/vst1 blocks, then 8-byte ones.
@ @test nl_copy r0=ptr r1=ptr r2=int:0:24 r3=int:0:4
    .global nl_copy
    .type nl_copy, %function
    .thumb_func
nl_copy:
    cbz     r2, 2f
1:  vld1.8  {d0, d1, d2, d3}, [r1]!
    vst1.8  {d0, d1, d2, d3}, [r0]!
    subs    r2, r2, #1
    bne     1b
2:  cbz     r3, 4f
3:  vld1.8  {d4}, [r1]!
    vst1.8  {d4}, [r0]!
    subs    r3, r3, #1
    bne     3b
4:  bx      lr
