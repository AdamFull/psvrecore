@ Synthetic corpus: hand-written code for the core registers in locals (ARCHITECTURE.md, "Registers"): registers
@ live across direct, indirect and tail calls; callees that preserve r4-r11 and sp by push/pop {.., pc}, by pop then
@ bx lr, by ldm sp! {.., pc} and mov pc, lr; a callee that changes a "callee-saved" register, one that saves it on some
@ paths only, one whose saved word is overwritten before the pop, one that pops into another register than it
@ pushed; sp moved by a run-time amount (alloca) and put back through a frame pointer, sp passed and returned; LR as
@ a scratch register, LR read as data, a return to another function than the caller (the saved LR replaced) and a
@ jump through a register; callbacks through psv_call that change callee-saved registers; recursion; 64-bit results
@ in r0:r1; stack arguments; a function entered directly and through the function table; conditional calls (Thumb
@ and ARM); a value from a call kept in a register across the next call of a loop (its copy pushed to memory);
@ vpush/vpop in a frame.

    .syntax unified
    .text

@ ---- Thumb callees ----
    .thumb

@ Preserves r4-r7: push {.., lr} / pop {.., pc}.
@ @test keep_pop_pc r0=any r1=any
    .global keep_pop_pc
    .type keep_pop_pc, %function
    .thumb_func
keep_pop_pc:
    push    {r4-r7, lr}
    add     r4, r0, r1
    eor     r5, r4, r0
    mul     r6, r5, r1
    sub     r7, r6, r4
    add     r0, r7, r5
    pop     {r4-r7, pc}

@ Preserves r4-r6: pop {.., lr} then bx lr.
@ @test keep_bx_lr r0=any r1=any
    .global keep_bx_lr
    .type keep_bx_lr, %function
    .thumb_func
keep_bx_lr:
    push    {r4-r6, lr}
    lsl     r4, r0, #3
    add     r5, r4, r1
    orr     r6, r5, r0
    add     r0, r6, #1
    pop     {r4-r6, lr}
    bx      lr

@ Changes r4 without saving it (hand-written code outside the calling convention).
    .global clobber_r4
    .type clobber_r4, %function
    .thumb_func
clobber_r4:
    add     r4, r0, #0x55
    add     r0, r0, r4
    bx      lr

@ Saves r5 on one path only; the other changes it.
@ @test save_some_paths r0=int:0:31 r1=any
    .global save_some_paths
    .type save_some_paths, %function
    .thumb_func
save_some_paths:
    cmp     r0, #16
    bhs     1f
    push    {r5, lr}
    mov     r5, r0
    add     r0, r5, r1
    pop     {r5, pc}
1:  add     r5, r0, r1
    mov     r0, r5
    bx      lr

@ Moves sp by a run-time amount; the frame pointer puts it back.
@ @test alloca_frame r0=any
    .global alloca_frame
    .type alloca_frame, %function
    .thumb_func
alloca_frame:
    push    {r4, r7, lr}
    add     r7, sp, #4
    and     r1, r0, #0x3C
    add     r1, r1, #8
    sub     sp, sp, r1
    mov     r4, sp
    str     r0, [r4]
    str     r1, [r4, #4]
    ldr     r2, [sp]
    add     r0, r2, r1
    sub     r4, r7, #4
    mov     sp, r4
    pop     {r4, r7, pc}

@ Returns the stack pointer it sees, and the word there.
    .global sp_value
    .type sp_value, %function
    .thumb_func
sp_value:
    mov     r0, sp
    ldr     r1, [sp]
    bx      lr

@ LR as a scratch register; the return through its saved copy.
@ @test lr_scratch r0=any r1=any
    .global lr_scratch
    .type lr_scratch, %function
    .thumb_func
lr_scratch:
    push    {r4, lr}
    add     lr, r0, r1
    eor     r4, lr, r0
    mov     r0, lr
    add     r0, r0, r4
    pop     {r4, pc}

@ Reads its return address as data.
    .global lr_as_data
    .type lr_as_data, %function
    .thumb_func
lr_as_data:
    mov     r2, lr
    eor     r0, r0, r2
    bx      lr

@ A 64-bit result in r0:r1.
    .global mul64
    .type mul64, %function
    .thumb_func
mul64:
    umull   r0, r1, r0, r1
    adds    r0, r0, #1
    adc     r1, r1, #0
    bx      lr

@ The fifth and sixth arguments at [sp] and [sp, #4].
    .global stack_args
    .type stack_args, %function
    .thumb_func
stack_args:
    ldr     r12, [sp]
    add     r0, r0, r12
    ldr     r12, [sp, #4]
    add     r0, r0, r12
    add     r0, r0, r1
    add     r0, r0, r2
    add     r0, r0, r3
    bx      lr

@ Overwrites the word r4 was saved in: r4 comes back changed.
@ @test overwrite_slot r0=any
    .global overwrite_slot
    .type overwrite_slot, %function
    .thumb_func
overwrite_slot:
    push    {r4, lr}
    str     r0, [sp]
    mov     r4, #9
    pop     {r4, pc}

@ Pops into other registers than it pushed: r4 and r5 come back swapped.
@ @test swap_restore r0=any r1=any
    .global swap_restore
    .type swap_restore, %function
    .thumb_func
swap_restore:
    push    {r4, r5, lr}
    mov     r4, r1
    mov     r5, r0
    pop     {r5}
    pop     {r4, pc}

@ d8 saved with vpush next to the core registers.
@ @test vfp_frame r0=any r1=any r2=any
    .global vfp_frame
    .type vfp_frame, %function
    .thumb_func
vfp_frame:
    push    {r4, lr}
    vpush   {d8}
    vmov    d8, r0, r1
    vmov    r4, r2, d8
    add     r0, r4, r2
    vpop    {d8}
    pop     {r4, pc}

@ Recursion that keeps a value in r4 across its own calls.
    .global sum_down
    .type sum_down, %function
    .thumb_func
sum_down:
    push    {r4, lr}
    movs    r4, r0
    beq     1f
    subs    r0, r0, #1
    bl      sum_down
    add     r0, r0, r4
    pop     {r4, pc}
1:  movs    r0, #0
    pop     {r4, pc}

@ Restores its frame, then a tail call.
@ @test tail_after_pop r0=any r1=any
    .global tail_after_pop
    .type tail_after_pop, %function
    .thumb_func
tail_after_pop:
    push    {r4, lr}
    add     r4, r0, #3
    mov     r0, r4
    pop     {r4, lr}
    b.w     keep_pop_pc

@ A callback that changes r4-r7.
    .global cb_clobber
    .type cb_clobber, %function
    .thumb_func
cb_clobber:
    mov     r4, r0
    add     r5, r0, #1
    add     r6, r0, #2
    add     r7, r0, #3
    add     r0, r4, r7
    bx      lr

@ A callback that reads r6 (its caller's, through the context).
    .global cb_reads_r6
    .type cb_reads_r6, %function
    .thumb_func
cb_reads_r6:
    add     r0, r0, r6
    eor     r0, r0, r6, lsl #3
    bx      lr

@ Entered directly and through the function table.
    .global both_ways
    .type both_ways, %function
    .thumb_func
both_ways:
    push    {r4, lr}
    add     r4, r0, r1
    eor     r0, r4, r1
    pop     {r4, pc}

@ r0 += 7, with r4 pushed (its saved copy is in the memory the test compares).
    .global next_value
    .type next_value, %function
    .thumb_func
next_value:
    push    {r4, lr}
    adds    r0, r0, #7
    pop     {r4, pc}

@ Its saved LR replaced: pop {pc} goes to keep_pop_pc (not to its caller), which returns to the caller.
@ @test return_elsewhere r0=any r1=any
    .global return_elsewhere
    .type return_elsewhere, %function
    .thumb_func
return_elsewhere:
    push    {r4, lr}
    ldr     r4, =keep_pop_pc
    str     r4, [sp, #4]
    add     r4, r0, #1
    pop     {r4, pc}
    .ltorg

@ Jumps through a register (not a constant to the analysis) to keep_bx_lr, which returns to the caller.
@ @test jump_through_register r0=any r1=any
    .global jump_through_register
    .type jump_through_register, %function
    .thumb_func
jump_through_register:
    push    {r4, lr}
    add     r4, r0, r1
    mov     r1, r4
    pop     {r4, lr}
    ldr     r3, =keep_bx_lr
    and     r2, r0, #0
    add     r3, r3, r2
    bx      r3
    .ltorg

@ A register changed before vmsr (whose translation writes registers to the context only on its way to the trap of
@ an FPSCR setting not implemented), then popped back to what the context still holds.
@ @test vmsr_restore r0=any
    .global vmsr_restore
    .type vmsr_restore, %function
    .thumb_func
vmsr_restore:
    push    {r4, lr}
    mov     r4, r0
    vmrs    r1, fpscr
    vmsr    fpscr, r1
    add     r0, r4, #1
    pop     {r4, pc}

@ ---- Thumb callers ----

@ Registers live across two direct calls to functions that preserve them.
@ @test across_direct r0=any r1=any
    .global across_direct
    .type across_direct, %function
    .thumb_func
across_direct:
    push    {r4-r8, lr}
    add     r4, r0, #1
    add     r5, r1, #2
    add     r8, r0, r1
    bl      keep_pop_pc
    add     r4, r4, r0
    bl      keep_bx_lr
    add     r5, r5, r0
    add     r0, r4, r5
    add     r0, r0, r8
    pop     {r4-r8, pc}

@ r4 across a call to a function that changes it.
@ @test across_clobber r0=any r1=any
    .global across_clobber
    .type across_clobber, %function
    .thumb_func
across_clobber:
    push    {r4, lr}
    mov     r4, r1
    bl      clobber_r4
    add     r0, r0, r4
    pop     {r4, pc}

@ r5 across a call that saves it on some paths only.
@ @test across_some_paths r0=int:0:31 r1=any
    .global across_some_paths
    .type across_some_paths, %function
    .thumb_func
across_some_paths:
    push    {r5, r6, lr}
    add     r5, r1, #7
    mov     r6, r0
    bl      save_some_paths
    add     r0, r0, r5
    add     r0, r0, r6
    pop     {r5, r6, pc}

@ sp and registers across an alloca frame.
@ @test across_alloca r0=any
    .global across_alloca
    .type across_alloca, %function
    .thumb_func
across_alloca:
    push    {r4, r5, lr}
    mov     r4, sp
    add     r5, r0, #1
    bl      alloca_frame
    mov     r1, sp
    sub     r1, r4, r1
    add     r0, r0, r1
    add     r0, r0, r5
    pop     {r4, r5, pc}

@ sp passed to a callee and returned by it.
@ @test sp_round_trip r0=any
    .global sp_round_trip
    .type sp_round_trip, %function
    .thumb_func
sp_round_trip:
    push    {r4, lr}
    sub     sp, sp, #8
    str     r0, [sp]
    bl      sp_value
    mov     r2, sp
    sub     r0, r0, r2
    add     r0, r0, r1
    add     sp, sp, #8
    pop     {r4, pc}

@ LR as scratch and as data in the callees.
@ @test lr_cases r0=any r1=any
    .global lr_cases
    .type lr_cases, %function
    .thumb_func
lr_cases:
    push    {r4, lr}
    bl      lr_scratch
    mov     r4, r0
    bl      lr_as_data
    eor     r0, r0, r4
    pop     {r4, pc}

@ A 64-bit result, and its halves used.
@ @test wide_results r0=any r1=any r2=any
    .global wide_results
    .type wide_results, %function
    .thumb_func
wide_results:
    push    {r4, r5, lr}
    mov     r4, r2
    bl      mul64
    add     r0, r0, r4
    eor     r1, r1, r4
    pop     {r4, r5, pc}

@ Arguments on the stack.
@ @test with_stack_args r0=any r1=any r2=any r3=any r4=any
    .global with_stack_args
    .type with_stack_args, %function
    .thumb_func
with_stack_args:
    push    {r4, r5, lr}
    sub     sp, sp, #12
    str     r4, [sp]
    add     r5, r4, #5
    str     r5, [sp, #4]
    bl      stack_args
    add     r0, r0, r5
    add     sp, sp, #12
    pop     {r4, r5, pc}

@ Callees whose restores are not what they saved.
@ @test slot_and_swap r0=any r1=any r4=any r5=any
    .global slot_and_swap
    .type slot_and_swap, %function
    .thumb_func
slot_and_swap:
    push    {r4, r5, r6, lr}
    mov     r6, r4
    bl      overwrite_slot
    add     r6, r6, r4
    mov     r0, r6
    bl      swap_restore
    sub     r0, r4, r5
    add     r0, r0, r6
    pop     {r4, r5, r6, pc}

@ A callee with d8 in its frame.
@ @test with_vfp_frame r0=any r1=any r2=any
    .global with_vfp_frame
    .type with_vfp_frame, %function
    .thumb_func
with_vfp_frame:
    push    {r4, lr}
    mov     r4, r0
    bl      vfp_frame
    add     r0, r0, r4
    pop     {r4, pc}

@ Recursion.
@ @test recursion r0=int:0:12
    .global recursion
    .type recursion, %function
    .thumb_func
recursion:
    push    {r4, lr}
    add     r4, r0, #100
    bl      sum_down
    add     r0, r0, r4
    pop     {r4, pc}

@ A callee that ends in a tail call.
@ @test tail_caller r0=any r1=any
    .global tail_caller
    .type tail_caller, %function
    .thumb_func
tail_caller:
    push    {r4, r5, lr}
    mov     r5, r1
    bl      tail_after_pop
    add     r0, r0, r5
    pop     {r4, r5, pc}

@ A callback through psv_call that changes r4-r7.
@ @test indirect_callback r0=any
    .global indirect_callback
    .type indirect_callback, %function
    .thumb_func
indirect_callback:
    push    {r4-r8, lr}
    mov     r8, r4
    add     r4, r0, #11
    ldr     r3, =cb_clobber
    blx     r3
    add     r0, r0, r4
    add     r0, r0, r5
    add     r0, r0, r8
    pop     {r4-r8, pc}
    .ltorg

@ A register written under a condition, then read by an indirect callee through the context.
@ @test cond_write_sync r0=any r1=any
    .global cond_write_sync
    .type cond_write_sync, %function
    .thumb_func
cond_write_sync:
    push    {r4, r6, lr}
    cmp     r0, r1
    it      hi
    movhi   r6, r0
    ldr     r3, =cb_reads_r6
    blx     r3
    add     r0, r0, r6
    pop     {r4, r6, pc}
    .ltorg

@ The same function called directly and through the function table.
@ @test direct_and_table r0=any r1=any
    .global direct_and_table
    .type direct_and_table, %function
    .thumb_func
direct_and_table:
    push    {r4, r5, lr}
    mov     r4, r0
    bl      both_ways
    mov     r5, r0
    mov     r0, r4
    ldr     r3, =both_ways
    blx     r3
    add     r0, r0, r5
    add     r0, r0, r4
    pop     {r4, r5, pc}
    .ltorg

@ Registers across calls to functions that do not return to their caller by the LR they were given.
@ @test calls_elsewhere r0=any r1=any
    .global calls_elsewhere
    .type calls_elsewhere, %function
    .thumb_func
calls_elsewhere:
    push    {r4, r5, r6, lr}
    add     r6, r0, #1
    mov     r5, r1
    bl      return_elsewhere
    add     r6, r6, r0
    mov     r0, r5
    mov     r1, r6
    bl      jump_through_register
    add     r0, r0, r6
    add     r0, r0, r4
    add     r0, r0, r5
    pop     {r4, r5, r6, pc}

@ Calls under a condition.
@ @test conditional_calls r0=any r1=any
    .global conditional_calls
    .type conditional_calls, %function
    .thumb_func
conditional_calls:
    push    {r4, r5, lr}
    mov     r5, r4
    cmp     r0, r1
    it      hi
    blhi    keep_pop_pc
    it      ls
    blls    clobber_r4
    add     r0, r0, r4
    add     r0, r0, r5
    pop     {r4, r5, pc}

@ A call's result moved to a register the next call (in the loop) pushes.
@ @test loop_kept r0=any
    .global loop_kept
    .type loop_kept, %function
    .thumb_func
loop_kept:
    push    {r4, r5, lr}
    movs    r5, #3
1:  bl      next_value
    mov     r4, r0
    subs    r5, r5, #1
    bne     1b
    add     r0, r0, r4
    pop     {r4, r5, pc}

@ ---- ARM ----
    .arm

@ Preserves r4-r11: stmfd/ldmfd sp!, {.., pc}.
@ @test arm_keep r0=any r1=any
    .global arm_keep
    .type arm_keep, %function
arm_keep:
    stmfd   sp!, {r4-r11, lr}
    add     r4, r0, r1
    add     r11, r4, r0
    eor     r8, r11, r1
    add     r0, r8, r11
    ldmfd   sp!, {r4-r11, pc}

@ Changes r8 without saving it.
    .global arm_clobber
    .type arm_clobber, %function
arm_clobber:
    add     r8, r0, #3
    mov     r0, r8
    bx      lr

@ Returns with mov pc, lr.
    .global arm_mov_pc
    .type arm_mov_pc, %function
arm_mov_pc:
    add     r0, r0, #1
    mov     pc, lr

@ Calls under a condition, in ARM code.
@ @test arm_conditional_calls r0=any r1=any
    .global arm_conditional_calls
    .type arm_conditional_calls, %function
arm_conditional_calls:
    push    {r4, r8, lr}
    mov     r4, r8
    cmp     r0, r1
    blhi    arm_keep
    blls    arm_clobber
    bl      arm_mov_pc
    add     r0, r0, r8
    add     r0, r0, r4
    pop     {r4, r8, pc}
