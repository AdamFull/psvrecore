@ Synthetic corpus: the condition flags where the generator's flag liveness has to get them right: flags returned by
@ a helper (vcmp + vmrs, a constant jump to a compare), flags read at entry through a direct call, an indirect call, a
@ tail call and an indirect jump, setters that execute under a condition and the flags where their paths join, a
@ carry through a loop and in from the caller, the shifter's carry read instructions later, flags through a jump
@ table, mrs and msr, a call under a condition to a function that never returns, and an address-table switch reached
@ with another table in its base register (the generated code's path for an index outside its table). See control.s.

    .syntax unified
    .text

@ ---- Thumb ----
    .thumb

@ A compare whose result is in the flags, as __aeabi_cfcmple's is.
    .global fcmp_flags
    .type fcmp_flags, %function
    .thumb_func
fcmp_flags:
    vcmp.f32 s0, s1
    vmrs    APSR_nzcv, fpscr
    bx      lr

@ @test flags_after_fcmp r0=any r1=any
    .global flags_after_fcmp
    .type flags_after_fcmp, %function
    .thumb_func
flags_after_fcmp:
    push    {r4, lr}
    vmov    s0, r0
    vmov    s1, r1
    mov     r4, r0
    bl      fcmp_flags
    ite     mi                       @ less
    movmi   r0, #1
    movpl   r0, #2
    it      vs                       @ unordered
    addvs   r0, r0, #10
    it      eq
    addeq   r0, r0, #100
    pop     {r4, pc}

    .global int_compare
    .type int_compare, %function
    .thumb_func
int_compare:
    cmp     r0, r1
    bx      lr

@ A veneer: a constant jump (ldr pc from a literal) to a compare that returns its result in the flags.
    .global veneer_compare
    .type veneer_compare, %function
    .thumb_func
    .p2align 2
veneer_compare:
    ldr.w   pc, 1f
    .p2align 2
1:  .word   int_compare

@ @test flags_through_veneer r0=any r1=any
    .global flags_through_veneer
    .type flags_through_veneer, %function
    .thumb_func
flags_through_veneer:
    push    {r4, lr}
    bl      veneer_compare
    ite     lt
    movlt   r0, #1
    movge   r0, #2
    it      cs
    addcs   r0, r0, #4
    pop     {r4, pc}

@ Reads the flags it is entered with (N and V, C).
    .global reads_entry_flags
    .type reads_entry_flags, %function
    .thumb_func
reads_entry_flags:
    ite     ge
    movge   r0, #3
    movlt   r0, #4
    it      cs
    addcs   r0, r0, #8
    bx      lr

@ @test entry_flags_direct r0=any r1=any
    .global entry_flags_direct
    .type entry_flags_direct, %function
    .thumb_func
entry_flags_direct:
    push    {r4, lr}
    cmp     r0, r1
    bl      reads_entry_flags
    pop     {r4, pc}

@ @test entry_flags_indirect r0=any r1=any
    .global entry_flags_indirect
    .type entry_flags_indirect, %function
    .thumb_func
entry_flags_indirect:
    push    {r4, lr}
    ldr     r3, =reads_entry_flags
    cmp     r0, r1
    blx     r3
    pop     {r4, pc}
    .ltorg

@ @test entry_flags_tail r0=any r1=any
    .global entry_flags_tail
    .type entry_flags_tail, %function
    .thumb_func
entry_flags_tail:
    cmp     r0, r1
    b.w     reads_entry_flags

@ (the target through another register: a jump to code the module does not know, as far as the analysis goes)
@ @test entry_flags_jump r0=any r1=any
    .global entry_flags_jump
    .type entry_flags_jump, %function
    .thumb_func
entry_flags_jump:
    ldr     r3, =reads_entry_flags
    mov     r12, r3
    cmp     r0, r1
    bx      r12
    .ltorg

@ Setters under a condition: where the paths join, the flags are those of whichever ran last.
@ @test conditional_setters r0=any r1=any r2=any r3=int:0:3
    .global conditional_setters
    .type conditional_setters, %function
    .thumb_func
conditional_setters:
    cmp     r3, #1
    itt     hi
    cmphi   r0, r1
    movhi   r3, #0
    beq     1f
    it      ls
    addsls.w r0, r0, r2
    b       2f
1:  subs    r0, r1, r2
2:  ite     cc
    movcc   r1, #1
    movcs   r1, #2
    adc     r0, r0, r1
    bx      lr

@ A multiword addition: the carry comes in from the caller, runs through the loop (whose test leaves it) and out.
@ @test carry_loop r0=ptr:4 r1=ptr:4 r2=int:1:12
    .global carry_loop
    .type carry_loop, %function
    .thumb_func
carry_loop:
    push    {r4, r5, lr}
1:  ldr     r4, [r0]
    ldr     r5, [r1], #4
    adcs    r4, r4, r5
    str     r4, [r0], #4
    sub.w   r2, r2, #1
    tst     r2, r2                   @ N and Z; the carry stays
    bne     1b
    ite     cs
    movcs   r0, #1
    movcc   r0, #0
    pop     {r4, r5, pc}

@ The shifter's carry, read some instructions later; a shift by a register (0-40) that may keep it.
@ @test shifter_carry r0=any r1=any r2=int:0:40
    .global shifter_carry
    .type shifter_carry, %function
    .thumb_func
shifter_carry:
    lsls    r3, r0, #1
    mov     r12, r1
    add.w   r1, r1, r0
    eor.w   r0, r0, r1
    adc     r0, r0, r3
    ands.w  r1, r1, r0, ror #5
    eor.w   r0, r0, r12
    asrs.w  r3, r1, r2
    sbc     r0, r0, r3
    it      cs
    addcs   r0, r0, #1
    bx      lr

@ The flags of a table branch's bounds check, read in its cases.
@ @test flags_through_table r0=int:0:6 r1=any
    .global flags_through_table
    .type flags_through_table, %function
    .thumb_func
flags_through_table:
    cmp     r0, #4
    bhi     9f
    tbb     [pc, r0]
3:  .byte   (10f - 3b) / 2, (11f - 3b) / 2, (12f - 3b) / 2, (11f - 3b) / 2
    .byte   (13f - 3b) / 2, 0
10: ite     eq
    moveq   r0, r1
    movne   r0, #7
    bx      lr
11: adc     r0, r1, #5               @ the compare's carry: clear below 4
    bx      lr
12: it      mi
    rsbmi   r1, r1, #0
    mov     r0, r1
    bx      lr
13: ite     eq                       @ index 4: equal
    addeq   r0, r1, #1
    movne   r0, #9
    bx      lr
9:  ite     hi
    movhi   r0, #0
    movls   r0, #1
    bx      lr

@ mrs and msr: the flags as a register, and back.
@ @test status_register r0=any r1=any
    .global status_register
    .type status_register, %function
    .thumb_func
status_register:
    adds    r2, r0, r1
    mrs     r3, apsr
    msr     APSR_nzcvq, r1
    ite     gt
    movgt   r0, r3
    movle   r0, #0
    adc     r0, r0, #0
    bx      lr

@ Never returns (a loop with no way out: code discovery finds it does not return).
    .global fatal_stop
    .type fatal_stop, %function
    .thumb_func
fatal_stop:
1:  b       1b

@ A call under a condition to a function that never returns: when the condition fails, the code goes on.
@ @test conditional_noreturn r0=int:0:0 r1=any
    .global conditional_noreturn
    .type conditional_noreturn, %function
    .thumb_func
conditional_noreturn:
    cmp     r0, #0
    it      ne
    blne    fatal_stop
    adds    r0, r1, #1
    bx      lr

@ ---- ARM ----
    .arm

@ Setters under a condition in ARM code.
@ @test arm_conditional_setters r0=any r1=any r2=any
    .global arm_conditional_setters
    .type arm_conditional_setters, %function
arm_conditional_setters:
    tst     r2, #1
    cmpne   r0, r1
    addseq  r0, r0, r2
    movmi   r0, #0
    adcs    r1, r1, r0
    rsbvs   r0, r1, #0
    bx      lr

@ An address-table switch that a second path reaches with another table (the same targets in another order) in its
@ base register: the generated code switches on the index only when the register holds its table's address, and
@ otherwise finds the target the guest loads among the table's.
@ @test table_other_base r0=int:0:3 r1=any
    .global table_other_base
    .type table_other_base, %function
table_other_base:
    cmp     r0, #3
    bhi     9f
    tst     r1, #1
    bne     5f
    adr     r2, 1f
4:  ldr     pc, [r2, r0, lsl #2]
1:  .word   10f, 11f, 12f, 13f
2:  .word   12f, 13f, 10f, 11f
5:  adr     r2, 2b
    b       4b
10: add     r0, r1, #1
    bx      lr
11: eor     r0, r1, #0xFF
    bx      lr
12: sub     r0, r1, #3
    bx      lr
13: mov     r0, r1, lsl #2
    bx      lr
9:  mov     r0, #0
    bx      lr
