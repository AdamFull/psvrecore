@ Synthetic corpus: hand-written control flow that compilers rarely emit but hand-written library code does:
@ condition flags live at a function's entry and across calls and returns (a compare helper that returns its result
@ in the flags, as __aeabi_cfcmple does; a carry chained through a call), returns by bx lr, pop {pc} and ldm with pc,
@ tail branches to other functions, TBB/TBH tables, long IT blocks, cbz/cbnz loops, conditional calls in ARM code.
@ See integer.c.

    .syntax unified
    .text

@ ---- Thumb ----
    .thumb

@ Reads the flags it is called with (no instruction before sets them).
@ @test flags_at_entry r0=any r1=any
    .global flags_at_entry
    .type flags_at_entry, %function
    .thumb_func
flags_at_entry:
    ite     hi
    addhi   r0, r0, r1
    subls   r0, r0, r1
    adcs    r0, r0, #0              @ the entry's carry
    it      vs
    eorvs   r0, r0, #0x5A
    bx      lr

@ Sets the flags for its caller: like __aeabi_cfcmple, the result is in NZCV.
    .global compare_in_flags
    .type compare_in_flags, %function
    .thumb_func
compare_in_flags:
    eor     r2, r0, #0x80000000
    eor     r3, r1, #0x80000000
    cmp     r2, r3                   @ unsigned order of the biased values: a signed compare
    bx      lr

@ @test flags_from_call r0=any r1=any r2=any
    .global flags_from_call
    .type flags_from_call, %function
    .thumb_func
flags_from_call:
    push    {r4, lr}
    mov     r4, r2
    bl      compare_in_flags
    ite     hi                       @ the flags the callee left
    movhi   r0, r4
    movls   r0, #1
    it      eq
    addeq   r0, r0, #100
    pop     {r4, pc}

@ A leaf that keeps the flags (no instruction in it writes them).
    .global no_flags_leaf
    .type no_flags_leaf, %function
    .thumb_func
no_flags_leaf:
    add     r0, r0, r1, lsl #1
    eor     r1, r1, r0
    bx      lr

@ The flags set before a call are tested after it; a carry chain runs across a call.
@ @test flags_across_call r0=any r1=any r2=any r3=any
    .global flags_across_call
    .type flags_across_call, %function
    .thumb_func
flags_across_call:
    push    {r4, r5, r6, lr}
    mov     r4, r2
    mov     r5, r3
    cmp     r0, r1
    bl      no_flags_leaf
    bge     1f
    adds    r4, r4, r5               @ sets the carry
    bl      no_flags_leaf
    adcs    r0, r0, r4               @ uses it
    pop     {r4, r5, r6, pc}
1:  subs    r4, r4, r5
    bl      no_flags_leaf
    sbcs    r0, r0, r4
    it      mi
    negmi   r0, r0
    pop     {r4, r5, r6, pc}

@ A counted loop with cbz/cbnz, a 4-instruction IT block, and a return by pop {pc}.
@ @test it_loop r0=ptr:4 r1=int:0:50
    .global it_loop
    .type it_loop, %function
    .thumb_func
it_loop:
    push    {r4, r5, lr}
    movs    r2, #0
    movs    r3, #0
    cbz     r1, 2f
1:  ldr     r4, [r0], #4
    cmp     r4, #0
    ittee   gt
    addgt   r2, r2, r4
    subgt.w r3, r3, #1               @ (32-bit encodings inside the block)
    addle   r3, r3, r4, asr #3
    eorle   r2, r2, r4
    subs    r1, #1
    bne     1b
    cbnz    r2, 2f
    movs    r3, #77
2:  adds    r0, r2, r3
    pop     {r4, r5, pc}

@ A tail branch into another function, which returns to the caller's caller.
@ @test tail_branch r0=any r1=any
    .global tail_branch
    .type tail_branch, %function
    .thumb_func
tail_branch:
    lsls    r1, r1, #2
    adds    r0, r0, #3
    cmp     r0, r1
    bhs.w   no_flags_leaf
    b.w     compare_in_flags         @ the flags it returns are the caller's flags

@ A hand-made TBB table with a default, and cases that share code.
@ @test tbb_table r0=int:0:10 r1=any
    .global tbb_table
    .type tbb_table, %function
    .thumb_func
tbb_table:
    cmp     r0, #6
    bhi     9f
    tbb     [pc, r0]
3:  .byte   (10f - 3b) / 2, (11f - 3b) / 2, (12f - 3b) / 2, (10f - 3b) / 2
    .byte   (13f - 3b) / 2, (14f - 3b) / 2, (12f - 3b) / 2, 0
10: adds    r0, r1, #1
    bx      lr
11: lsls    r0, r1, #3
    bx      lr
12: eors    r0, r1
    adds    r0, #9
    bx      lr
13: mvns    r0, r1
    bx      lr
14: asrs    r0, r1, #2
    b       15f
9:  movs    r0, #0
15: bx      lr

@ A TBH table whose cases are too far apart for TBB.
@ @test tbh_table r0=int:0:6 r1=any
    .global tbh_table
    .type tbh_table, %function
    .thumb_func
tbh_table:
    cmp     r0, #3
    bhi     29f
    tbh     [pc, r0, lsl #1]
20: .hword  (21f - 20b) / 2, (22f - 20b) / 2, (23f - 20b) / 2, (24f - 20b) / 2
21: adds    r0, r1, #11
    bx      lr
    .space  600, 0                   @ (never reached: discovery does not decode it)
22: subs    r0, r1, #5
    bx      lr
    .space  600, 0
23: lsrs    r0, r1, #1
    bx      lr
24: movs    r0, #4
    bx      lr
29: mov     r0, r1
    bx      lr

@ An ARM function called from Thumb, returning by bx lr into Thumb, and a Thumb function returning to ARM.
@ @test thumb_to_arm r0=any r1=any
    .global thumb_to_arm
    .type thumb_to_arm, %function
    .thumb_func
thumb_to_arm:
    push    {r4, lr}
    mov     r4, r0
    blx     arm_leaf
    add     r0, r0, r4
    pop     {r4, pc}

    .global thumb_leaf
    .type thumb_leaf, %function
    .thumb_func
thumb_leaf:
    rors    r0, r1
    bx      lr

@ ---- ARM ----
    .arm

    .global arm_leaf
    .type arm_leaf, %function
arm_leaf:
    cmp     r0, r1
    movgt   r0, r1
    addle   r0, r0, r1, lsl #2
    bx      lr

@ Conditional calls, conditional returns, ldm with pc, and a call into Thumb code.
@ @test arm_conditional_calls r0=any r1=int:0:40 r2=any
    .global arm_conditional_calls
    .type arm_conditional_calls, %function
arm_conditional_calls:
    push    {r4, r5, r6, lr}
    mov     r4, r0
    mov     r5, r1
    mov     r6, r2
    tst     r5, #1
    moveq   r0, r6
    moveq   r1, r5
    bleq    arm_leaf                 @ a call under a condition
    addne   r0, r4, r5
    cmp     r5, #20
    ldmhi   sp!, {r4, r5, r6, pc}    @ a conditional return
    mov     r1, r4
    blx     thumb_leaf               @ into Thumb
    subs    r0, r0, r6
    rsbmi   r0, r0, #0
    ldm     sp!, {r4, r5, r6, pc}

@ A loop whose flags come from the loop counter and survive a call: subs ... bl ... bne.
@ @test arm_flags_loop r0=any r1=int:1:30
    .global arm_flags_loop
    .type arm_flags_loop, %function
arm_flags_loop:
    push    {r4, r5, r11, lr}
    mov     r4, r0
    mov     r5, r1
1:  mov     r0, r4
    mov     r1, r5
    subs    r5, r5, #1
    bl      arm_keeps_flags
    mov     r4, r0
    bne     1b                       @ the flags of the subs, across the call
    mov     r0, r4
    pop     {r4, r5, r11, pc}

    .global arm_keeps_flags
    .type arm_keeps_flags, %function
arm_keeps_flags:
    eor     r0, r0, r1, ror #7
    add     r0, r0, r0, lsl #3
    bx      lr

@ A tail branch from ARM to a Thumb function (bx to an address with bit 0 set).
@ @test arm_tail_to_thumb r0=any r1=any
    .global arm_tail_to_thumb
    .type arm_tail_to_thumb, %function
arm_tail_to_thumb:
    add     r0, r0, #1
    ldr     r12, =thumb_leaf
    bx      r12
    .ltorg
