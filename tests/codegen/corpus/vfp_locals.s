@ Synthetic corpus: hand-written code for the floating-point registers in locals (ARCHITECTURE.md, "VFP
@ registers"): S and D registers of one D register written and read both ways; a D register written in part (an S
@ half, a lane); values live across direct calls (to a callee that touches no floating-point register, one that
@ saves d8 with vpush/vpop and changes d0, one that changes d8 and s4 without saving them, outside the calling
@ convention), across an indirect call and a tail call; vpush/vpop and vldm/vstm of S and D lists; NaN payloads
@ (signalling ones included) through registers, core registers, memory and literals; vmsr changing flush-to-zero and
@ default NaN mid-function and in a callee; vcmp + vmrs + branch with the flags read again later and across a call,
@ with the compare apart from the vmrs, compares with #0 and of doubles; Advanced SIMD and VFP on the same D
@ registers in one function; every conversion and arithmetic operation in both precisions; a double-held register
@ written in part; every condition code after a fused compare, on equal and unordered operands. The FPSCR the tests start
@ with is random in FZ and DN (round to nearest), and every register random: special values included.

    .syntax unified
    .fpu neon-fp16
    .text
    .thumb

@ ---- callees ----

@ Touches no floating-point register (a caller keeps every local across it).
    .global int_only
    .type int_only, %function
    .thumb_func
int_only:
    add     r0, r0, #3
    eor     r0, r0, r0, lsl #5
    bx      lr

@ Saves d8, changes it, puts it back; changes d0 (a result).
    .global saves_d8
    .type saves_d8, %function
    .thumb_func
saves_d8:
    vpush   {d8}
    vmov.f64 d8, #1.0
    vadd.f64 d0, d0, d8
    vmul.f64 d8, d8, d0
    vpop    {d8}
    bx      lr

@ Changes d8 and s4 without saving them (outside AAPCS: d8 is callee-saved there).
    .global clobbers_d8_s4
    .type clobbers_d8_s4, %function
    .thumb_func
clobbers_d8_s4:
    vmov.f32 s4, #2.0
    vmov.f64 d8, #3.0
    vadd.f32 s4, s4, s0
    bx      lr

@ Sets FPSCR's FZ and DN from r0 ^ 3 (bits 1-0), the rest kept.
    .global set_mode
    .type set_mode, %function
    .thumb_func
set_mode:
    vmrs    r1, fpscr
    bic     r1, r1, #0x03000000
    eor     r2, r0, #3
    and     r2, r2, #3
    orr     r1, r1, r2, lsl #24
    vmsr    fpscr, r1
    bx      lr

@ ---- tests ----

@ s0 and s1 written, d0 read (as bits and as a double); d1 written, its halves read; part of d4 written.
@ @test alias_sd r0=any
    .global alias_sd
    .type alias_sd, %function
    .thumb_func
alias_sd:
    vmov.f32 s0, #1.5
    vadd.f32 s1, s4, s5
    vmov    r0, r1, d0
    vadd.f64 d1, d0, d2
    vmul.f32 s6, s2, s3
    vmov.f64 d4, d1
    vneg.f32 s9, s2
    vmov    r2, r3, d4
    vmov    d5, r0, r3
    vsub.f32 s12, s10, s11
    vmov    s13, r1
    vabs.f64 d7, d6
    bx      lr

@ NaN payloads (and every other bit pattern) through S and D registers, core registers, memory, lists and literals.
@ @test nan_moves r0=fptr
    .global nan_moves
    .type nan_moves, %function
    .thumb_func
nan_moves:
    vldr    s0, [r0]
    vldr    d1, [r0, #8]
    vmov.f32 s4, s0
    vmov.f64 d3, d1
    vstr    s4, [r0, #16]
    vstr    d3, [r0, #24]
    vmov    r1, s4
    vmov    r2, r3, d3
    vmov    s5, r1
    vmov    d5, r2, r3
    vldmia  r0, {s8-s11}
    vstmdb  r0!, {s8-s11}
    vldmia  r0!, {d6-d7}
    vldr    s14, .Lsnan32
    vldr    d8, .Lsnan64
    vmov    r1, r2, d8
    vmov    s15, s14
    vstr    s15, [r0, #32]
    vstr    d8, [r0, #40]
    vmov    s16, s17, r1, r3
    vmov    r3, r12, s16, s17
    bx      lr
    .p2align 3
.Lsnan64:
    .word   0x00000001, 0x7FF00000
.Lsnan32:
    .word   0xFF800ABC

@ Values live across direct calls: to one that touches nothing, one that saves d8 and changes d0, one that changes d8
@ and s4; in d8-d9 (saved by vpush in the frame) and s4; a loop.
@ @test keep_across_calls r0=int:0:4
    .global keep_across_calls
    .type keep_across_calls, %function
    .thumb_func
keep_across_calls:
    push    {r4, lr}
    vpush   {d8-d9}
    vadd.f32 s16, s0, s1
    vmov.f64 d9, d2
    vmul.f32 s4, s0, s0
    mov     r4, r0
1:  bl      int_only
    vadd.f32 s4, s4, s16
    bl      saves_d8
    vadd.f64 d9, d9, d8
    vadd.f64 d10, d0, d9
    bl      clobbers_d8_s4
    vadd.f32 s5, s4, s17
    vsub.f64 d11, d8, d10
    subs    r4, #1
    bge     1b
    vmov    r0, s5
    vpop    {d8-d9}
    pop     {r4, pc}

@ Values live across an indirect call (psv_call) and a tail call, dirty when they happen.
@ @test across_indirect r0=any
    .global across_indirect
    .type across_indirect, %function
    .thumb_func
across_indirect:
    push    {r4, lr}
    vadd.f32 s0, s1, s2
    vmov.f64 d3, d4
    movw    r4, #:lower16:clobbers_d8_s4
    movt    r4, #:upper16:clobbers_d8_s4
    blx     r4
    vadd.f32 s6, s0, s4
    vmul.f64 d8, d8, d3
    pop     {r4, lr}
    vsub.f32 s7, s6, s1
    b       saves_d8

@ vmsr: flush-to-zero and default NaN set from r0 mid-function, the arithmetic in that mode, then a callee's mode;
@ the FPSCR put back at the end.
@ @test vmsr_modes r0=int:0:3
    .global vmsr_modes
    .type vmsr_modes, %function
    .thumb_func
vmsr_modes:
    vmrs    r2, fpscr
    push    {r2, lr}
    bic     r3, r2, #0x03000000
    orr     r3, r3, r0, lsl #24
    vmsr    fpscr, r3
    vmul.f32 s0, s1, s2
    vadd.f64 d2, d3, d4
    vcmp.f32 s0, s5
    vmrs    APSR_nzcv, fpscr
    ite     gt
    movgt   r3, #1
    movle   r3, #2
    bl      set_mode
    vmul.f32 s3, s1, s2
    vdiv.f64 d5, d3, d4
    vcvt.f32.f64 s12, d5
    vcmpe.f32 s3, #0
    vmrs    APSR_nzcv, fpscr
    it      mi
    addmi   r3, r3, #4
    vmrs    r1, fpscr
    pop     {r2, lr}
    vmsr    fpscr, r2
    vsqrt.f32 s14, s1
    mov     r0, r3
    bx      lr

@ vcmp + vmrs + branch, with the flags read again in another block and after a call; the compare apart from the
@ vmrs; doubles; #0.
@ @test cmp_flags_later r0=any
    .global cmp_flags_later
    .type cmp_flags_later, %function
    .thumb_func
cmp_flags_later:
    push    {r4, lr}
    vcmp.f32 s0, s1
    vmrs    APSR_nzcv, fpscr
    bgt     1f
    mov     r0, #0
    b       2f
1:  mov     r0, #1
2:  ite     vs
    movvs   r1, #1
    movvc   r1, #0
    it      ls
    addls   r0, r0, #4
    vcmpe.f64 d2, d3
    vmov.f32 s8, s9
    add     r4, r0, r1
    vmrs    APSR_nzcv, fpscr
    bl      int_only
    it      ge
    addge   r4, r4, #16
    vcmp.f64 d4, #0
    vmrs    APSR_nzcv, fpscr
    bhi     3f
    it      eq
    addeq   r4, r4, #32
    it      lt
    addlt   r4, r4, #64
3:  vmrs    r2, fpscr
    mov     r0, r4
    pop     {r4, pc}

@ A fused compare in a loop: counts the values of an array above, below and unordered against s0.
@ @test cmp_loop r0=fptr r1=int:0:48
    .global cmp_loop
    .type cmp_loop, %function
    .thumb_func
cmp_loop:
    movs    r2, #0
    movs    r3, #0
    cbz     r1, 3f
1:  vldmia  r0!, {s2}
    vcmpe.f32 s2, s0
    vmrs    APSR_nzcv, fpscr
    bvs     2f
    ite     gt
    addgt   r2, r2, #1
    addle   r3, r3, #1
    subs    r1, #1
    bne     1b
    b       3f
2:  add     r3, r3, #0x100
    subs    r1, #1
    bne     1b
3:  orr     r0, r2, r3, lsl #16
    bx      lr

@ Advanced SIMD and VFP on the same D registers: VFP writes s0, Advanced SIMD reads and writes d0 and q1, VFP reads
@ them back; scalar moves of words, halfwords and bytes.
@ @test mixed_neon r0=any
    .global mixed_neon
    .type mixed_neon, %function
    .thumb_func
mixed_neon:
    vadd.f32 s0, s0, s1
    vadd.i32 d0, d0, d1
    vmul.f32 s2, s0, s1
    vmov.32 r0, d0[1]
    vmov.32 d1[0], r0
    vadd.f32 q1, q1, q0
    vcvt.f64.f32 d4, s4
    vmov.8  d5[3], r0
    vmov.u16 r1, d5[1]
    vmov.s8 r2, d5[3]
    vmov.16 d6[2], r1
    veor    d7, d6, d5
    vsub.f32 s15, s14, s12
    vmov.32 r3, d7[1]
    vadd.f64 d16, d4, d7
    vmov.i32 q9, #0x40
    vmul.f64 d17, d16, d18
    bx      lr

@ Every conversion: to and from integers (toward zero and to nearest, signed and unsigned), between precisions, fixed
@ point of 16 and 32 bits in both precisions, half precision.
@ @test cvt_all r0=any
    .global cvt_all
    .type cvt_all, %function
    .thumb_func
cvt_all:
    vcvt.s32.f32  s0, s1
    vcvtr.s32.f32 s2, s3
    vcvt.u32.f32  s4, s5
    vcvtr.u32.f32 s6, s7
    vcvt.s32.f64  s8, d5
    vcvtr.u32.f64 s9, d6
    vcvt.u32.f64  s10, d16
    vcvtr.s32.f64 s11, d17
    vcvt.f32.s32  s12, s13
    vcvt.f32.u32  s14, s15
    vcvt.f64.s32  d18, s16
    vcvt.f64.u32  d19, s17
    vcvt.f64.f32  d20, s18
    vcvt.f32.f64  s19, d21
    vcvt.s32.f32  s20, s20, #16
    vcvt.u16.f32  s21, s21, #8
    vcvt.f32.s16  s22, s22, #4
    vcvt.f32.u32  s23, s23, #32
    vcvt.f64.u32  d22, d22, #20
    vcvt.s16.f64  d23, d23, #3
    vcvt.u32.f64  d24, d24, #1
    vcvt.f64.s16  d25, d25, #15
    vcvtb.f16.f32 s24, s25
    vcvtt.f16.f32 s24, s26
    vcvtt.f32.f16 s27, s28
    vcvtb.f32.f16 s29, s28
    bx      lr

@ Every arithmetic operation, single and double precision, with operands of D-held and S-held registers.
@ @test arith_all r0=any
    .global arith_all
    .type arith_all, %function
    .thumb_func
arith_all:
    vadd.f32  s0, s1, s2
    vsub.f32  s3, s4, s5
    vmul.f32  s6, s7, s8
    vdiv.f32  s9, s10, s11
    vsqrt.f32 s12, s13
    vabs.f32  s14, s15
    vneg.f32  s16, s17
    vnmul.f32 s18, s19, s20
    vmla.f32  s21, s22, s23
    vmls.f32  s24, s25, s26
    vnmla.f32 s27, s28, s29
    vnmls.f32 s30, s31, s0
    vadd.f64  d16, d17, d18
    vsub.f64  d19, d20, d21
    vmul.f64  d22, d23, d24
    vdiv.f64  d25, d26, d27
    vsqrt.f64 d28, d29
    vabs.f64  d30, d31
    vneg.f64  d1, d2
    vnmul.f64 d3, d4, d5
    vmla.f64  d6, d7, d8
    vmls.f64  d9, d10, d11
    vnmla.f64 d12, d13, d14
    vnmls.f64 d15, d16, d17
    vmov      r0, r1, d0
    vmul.f32  s1, s0, s1
    bx        lr

@ A D register held as a double (accessed whole more often than by word) written in its low half only, its high half
@ the one it had at entry: the partial write reads the whole register.
@ @test partial_d r0=any
    .global partial_d
    .type partial_d, %function
    .thumb_func
partial_d:
    vmov.f32 s6, #1.0
    vadd.f64 d4, d3, d3
    vmul.f64 d5, d3, d4
    bx      lr

@ Every condition after a fused compare, on operands that are often equal (small integers) or unordered (the square
@ root of a negative): each condition sets its own bit of r0.
@ @test cond_all r0=int:0:3 r1=int:0:3
    .global cond_all
    .type cond_all, %function
    .thumb_func
cond_all:
    vmov    s0, r0
    vmov    s1, r1
    vcvt.f32.s32 s0, s0
    vcvt.f32.s32 s1, s1
    vmov.f32 s2, #2.0
    vsub.f32 s3, s0, s2
    vsqrt.f32 s3, s3
    mov     r2, #0
    vcmp.f32 s0, s1
    vmrs    APSR_nzcv, fpscr
    it eq
    orreq   r2, r2, #0x1
    it ne
    orrne   r2, r2, #0x2
    it cs
    orrcs   r2, r2, #0x4
    it cc
    orrcc   r2, r2, #0x8
    it mi
    orrmi   r2, r2, #0x10
    it pl
    orrpl   r2, r2, #0x20
    it vs
    orrvs   r2, r2, #0x40
    it vc
    orrvc   r2, r2, #0x80
    it hi
    orrhi   r2, r2, #0x100
    it ls
    orrls   r2, r2, #0x200
    it ge
    orrge   r2, r2, #0x400
    it lt
    orrlt   r2, r2, #0x800
    it gt
    orrgt   r2, r2, #0x1000
    it le
    orrle   r2, r2, #0x2000
    vcvt.f64.f32 d4, s3
    vcvt.f64.f32 d5, s1
    vcmpe.f64 d4, d5
    vmrs    APSR_nzcv, fpscr
    it eq
    orreq   r2, r2, #0x10000
    it ne
    orrne   r2, r2, #0x20000
    it hi
    orrhi   r2, r2, #0x40000
    it ls
    orrls   r2, r2, #0x80000
    it ge
    orrge   r2, r2, #0x100000
    it lt
    orrlt   r2, r2, #0x200000
    it gt
    orrgt   r2, r2, #0x400000
    it le
    orrle   r2, r2, #0x800000
    it vs
    orrvs   r2, r2, #0x1000000
    mov     r0, r2
    bx      lr
