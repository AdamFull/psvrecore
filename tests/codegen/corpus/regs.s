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

@ ---- Internal entries (ARCHITECTURE.md, "Internal entries") ----
@ Direct calls between generated functions pass r0-r3, sp and lr as C arguments and get r0/r1 back; callees with
@ nothing outside in their reach are called without the context. Argument and result registers in every
@ combination, results in r2/r3/r12 and in the flags, r4/r5 read at entry, stack arguments and a pointer into the
@ caller's frame, VFP arguments and results, a deep chain of internal calls ending in a call through psv_call (the
@ test checks the whole context there against the interpreter: functions named obs_*), a callee that puts back a
@ register its own callee changed, recursion and mutual recursion, a function entered through its internal entry
@ and through the table in one run, a tail-call chain, a jump through the runtime with another LR than ret.
    .thumb

@ Reads r0-r3, returns r0.
    .global ie_sum4
    .type ie_sum4, %function
    .thumb_func
ie_sum4:
    add     r0, r0, r1
    add     r2, r2, r3
    eor     r0, r0, r2
    bx      lr

@ A 64-bit result in r0:r1.
    .global ie_mul64
    .type ie_mul64, %function
    .thumb_func
ie_mul64:
    umull   r0, r1, r0, r1
    bx      lr

@ Results in r2, r3 and r12 (outside AAPCS); r0 and r1 unchanged.
    .global ie_scratch_out
    .type ie_scratch_out, %function
    .thumb_func
ie_scratch_out:
    add     r2, r0, #7
    eor     r3, r1, r0
    add     r12, r2, r3
    bx      lr

@ Reads r4 and r5 at entry (outside AAPCS); changes r5 without saving it.
    .global ie_reads_r4
    .type ie_reads_r4, %function
    .thumb_func
ie_reads_r4:
    add     r0, r4, r5
    add     r5, r5, #1
    bx      lr

@ Stack arguments.
    .global ie_stack_args
    .type ie_stack_args, %function
    .thumb_func
ie_stack_args:
    ldr     r2, [sp]
    ldr     r3, [sp, #4]
    add     r0, r0, r2
    add     r0, r0, r3
    bx      lr

@ A pointer into its caller's frame: stores through it and reads it back.
    .global ie_store_through
    .type ie_store_through, %function
    .thumb_func
ie_store_through:
    str     r1, [r0]
    ldr     r2, [r0]
    add     r0, r2, #1
    bx      lr

@ Changes r1 on one path only.
    .global ie_some_paths
    .type ie_some_paths, %function
    .thumb_func
ie_some_paths:
    cmp     r0, #8
    it      hs
    addhs   r1, r0, #3
    add     r0, r0, #1
    bx      lr

@ Leaves its result in the flags.
    .global ie_flags_out
    .type ie_flags_out, %function
    .thumb_func
ie_flags_out:
    cmp     r0, r1
    bx      lr

@ Every one of the above from one caller.
@ @test ie_caller r0=any r1=any r2=any r3=any
    .global ie_caller
    .type ie_caller, %function
    .thumb_func
ie_caller:
    push    {r4-r7, lr}
    sub     sp, sp, #16
    mov     r4, r0
    mov     r5, r1
    mov     r6, r2
    mov     r7, r3
    bl      ie_sum4
    mov     r1, r5
    bl      ie_mul64
    eor     r4, r4, r1
    add     r4, r4, r0
    mov     r0, r6
    mov     r1, r7
    bl      ie_scratch_out
    add     r4, r4, r2
    add     r4, r4, r3
    add     r4, r4, r12
    mov     r5, r6
    bl      ie_reads_r4
    add     r4, r4, r5
    eor     r4, r4, r0
    str     r6, [sp]
    str     r7, [sp, #4]
    mov     r0, r4
    bl      ie_stack_args
    add     r4, r4, r0
    add     r0, sp, #8
    mov     r1, r4
    bl      ie_store_through
    ldr     r1, [sp, #8]
    add     r4, r4, r1
    and     r0, r7, #15
    mov     r1, r6
    bl      ie_some_paths
    add     r4, r4, r1
    add     r4, r4, r0
    mov     r0, r6
    mov     r1, r7
    bl      ie_flags_out
    ite     hi
    addhi   r4, r4, #5
    subls   r4, r4, #9
    mov     r0, r4
    add     sp, sp, #16
    pop     {r4-r7, pc}

@ VFP arguments and results: s0-s2 and d1-d2 in, s0 and d1 out.
    .global ie_vfp_leaf
    .type ie_vfp_leaf, %function
    .thumb_func
ie_vfp_leaf:
    vmul.f32 s0, s0, s1
    vadd.f32 s0, s0, s2
    vadd.f64 d1, d1, d2
    bx      lr

@ @test ie_vfp_caller r0=any
    .global ie_vfp_caller
    .type ie_vfp_caller, %function
    .thumb_func
ie_vfp_caller:
    push    {r4, lr}
    vpush   {d8}
    vmov.f32 s16, s0
    bl      ie_vfp_leaf
    vadd.f32 s0, s0, s16
    vmov.f32 s1, s16
    bl      ie_vfp_leaf
    vmov    r0, s0
    vpop    {d8}
    pop     {r4, pc}

@ Reached only through psv_call: the test compares the context at its entry with the interpreter's.
    .global obs_reads_all
    .type obs_reads_all, %function
    .thumb_func
obs_reads_all:
    add     r0, r0, r1
    add     r0, r0, r2
    add     r0, r0, r4
    add     r0, r0, r12
    bx      lr

@ The bottom of a chain: a call through psv_call.
    .global ie_chain_low
    .type ie_chain_low, %function
    .thumb_func
ie_chain_low:
    push    {r4, lr}
    ldr     r3, =obs_reads_all
    blx     r3
    add     r0, r0, #3
    pop     {r4, pc}
    .ltorg

@ The middle: touches none of r1, r2, r4-r12.
    .global ie_chain_mid
    .type ie_chain_mid, %function
    .thumb_func
ie_chain_mid:
    push    {r3, lr}
    add     r0, r0, #1
    bl      ie_chain_low
    add     r0, r0, #2
    pop     {r3, pc}

@ A middle with nothing outside in its reach.
    .global ie_inside_mid
    .type ie_inside_mid, %function
    .thumb_func
ie_inside_mid:
    push    {r3, lr}
    bl      ie_reads_r4
    add     r0, r0, #5
    pop     {r3, pc}

@ The top: r1, r2, r4-r8 and r12 written before the chain, so the context must hold them at the call through psv_call.
@ @test ie_chain_top r0=any r1=any
    .global ie_chain_top
    .type ie_chain_top, %function
    .thumb_func
ie_chain_top:
    push    {r4-r8, lr}
    mov     r4, r0
    add     r5, r0, r1
    eor     r6, r0, r1
    movs    r7, #0x77
    add     r8, r1, #3
    add     r2, r1, #9
    eor     r12, r0, #0x5A
    eor     r1, r1, #0xA5
    mov     r0, r5
    bl      ie_chain_mid
    add     r0, r0, r4
    add     r0, r0, r5
    mov     r4, r0
    add     r5, r5, #1
    bl      ie_inside_mid
    add     r0, r0, r6
    add     r0, r0, r7
    add     r0, r0, r8
    add     r1, r0, #1
    bl      ie_chain_mid
    pop     {r4-r8, pc}

@ Changes r4 without saving it; its caller below puts it back.
    .global ie_clobber_r4
    .type ie_clobber_r4, %function
    .thumb_func
ie_clobber_r4:
    add     r4, r0, #0x33
    add     r0, r0, r4
    bx      lr

@ Preserves r4 by push/pop around a callee that changes it.
    .global ie_saves_r4
    .type ie_saves_r4, %function
    .thumb_func
ie_saves_r4:
    push    {r4, lr}
    mov     r4, r1
    bl      ie_clobber_r4
    add     r0, r0, r4
    pop     {r4, pc}

@ After ie_saves_r4 the context must hold r4 as it was, not ie_clobber_r4's value: the call through psv_call reads it.
@ @test ie_restore_check r0=any r1=any
    .global ie_restore_check
    .type ie_restore_check, %function
    .thumb_func
ie_restore_check:
    push    {r4, lr}
    bl      ie_saves_r4
    ldr     r3, =obs_reads_all
    blx     r3
    pop     {r4, pc}
    .ltorg

@ Recursion through the internal entry.
@ @test ie_fact r0=int:0:12
    .global ie_fact
    .type ie_fact, %function
    .thumb_func
ie_fact:
    push    {r4, lr}
    mov     r4, r0
    cmp     r0, #1
    bls     1f
    sub     r0, r0, #1
    bl      ie_fact
    mul     r0, r0, r4
    pop     {r4, pc}
1:  movs    r0, #1
    pop     {r4, pc}

@ Mutual recursion.
@ @test ie_even r0=int:0:40
    .global ie_even
    .type ie_even, %function
    .thumb_func
ie_even:
    cbz     r0, 1f
    push    {r4, lr}
    sub     r0, r0, #1
    bl      ie_odd
    pop     {r4, pc}
1:  movs    r0, #1
    bx      lr

    .global ie_odd
    .type ie_odd, %function
    .thumb_func
ie_odd:
    cbz     r0, 1f
    push    {r4, lr}
    sub     r0, r0, #1
    bl      ie_even
    pop     {r4, pc}
1:  movs    r0, #0
    bx      lr

@ The same function through its internal entry and through the table (its f_ wrapper) in one run.
@ @test ie_both_ways r0=any r1=any r2=any r3=any
    .global ie_both_ways
    .type ie_both_ways, %function
    .thumb_func
ie_both_ways:
    push    {r4-r7, lr}
    mov     r4, r0
    mov     r5, r1
    mov     r6, r2
    mov     r7, r3
    bl      ie_sum4
    mov     r1, r0
    mov     r0, r4
    mov     r2, r6
    mov     r3, r7
    ldr     r4, =ie_sum4
    blx     r4
    add     r0, r0, r5
    pop     {r4-r7, pc}
    .ltorg

@ A tail-call chain into a function with an internal entry.
    .global ie_tail_b
    .type ie_tail_b, %function
    .thumb_func
ie_tail_b:
    add     r0, r0, #2
    b       ie_sum4

    .global ie_tail_a
    .type ie_tail_a, %function
    .thumb_func
ie_tail_a:
    add     r0, r0, #1
    b       ie_tail_b

@ @test ie_tail_caller r0=any r1=any r2=any r3=any
    .global ie_tail_caller
    .type ie_tail_caller, %function
    .thumb_func
ie_tail_caller:
    push    {r4, lr}
    bl      ie_tail_a
    add     r0, r0, #1
    pop     {r4, pc}

@ A jump through the runtime into ie_sum4 (its f_ wrapper) with LR another function: ie_sum4 returns there.
@ @test ie_lr_elsewhere r0=any r1=any r2=any r3=any
    .global ie_lr_elsewhere
    .type ie_lr_elsewhere, %function
    .thumb_func
ie_lr_elsewhere:
    push    {r4, lr}
    ldr     lr, =ie_landing
    ldr     r4, =ie_sum4
    bx      r4
    .ltorg

    .global ie_landing
    .type ie_landing, %function
    .thumb_func
ie_landing:
    add     r0, r0, #0x100
    pop     {r4, pc}

@ Inlined callees (small internal entries: a static copy in each file that calls them): one that may trap (its vmsr,
@ on an FPSCR setting not implemented; never on the test's), called twice; one the test also replaces with override_function (codegen_tests.cpp: the direct
@ call still reaches the original, the call through psv_call the replacement).
    .global ie_may_trap
    .type ie_may_trap, %function
    .thumb_func
ie_may_trap:
    vmrs    r2, fpscr
    add     r0, r0, r1
    vmsr    fpscr, r2
    bx      lr

@ @test ie_inlined_trap r0=any r1=any
    .global ie_inlined_trap
    .type ie_inlined_trap, %function
    .thumb_func
ie_inlined_trap:
    push    {r4, lr}
    mov     r4, r1
    bl      ie_may_trap
    mov     r1, r4
    bl      ie_may_trap
    pop     {r4, pc}

    .global ie_ov_leaf
    .type ie_ov_leaf, %function
    .thumb_func
ie_ov_leaf:
    add     r0, r0, #1
    bx      lr

@ @test ie_ov_caller r0=any
    .global ie_ov_caller
    .type ie_ov_caller, %function
    .thumb_func
ie_ov_caller:
    push    {r4, lr}
    bl      ie_ov_leaf
    mov     r4, r0
    ldr     r3, =ie_ov_leaf
    blx     r3
    add     r0, r0, r4
    pop     {r4, pc}
    .ltorg

@ ---- Inside calls that leave dirty registers out of the context ----
@ Before a call of an Inside internal entry the caller writes to the context only the dirty registers the callee's
@ reach reads or writes there, or whose value code after the call may read there before the caller writes them again
@ (ARCHITECTURE.md, "Internal entries"): before cr_mid_r4 r4 (read by the callee), r6 (read by the callee's callee)
@ and r7 (changed under a condition after the call: the old value reaches the psv_call on one path), not r5 or r8,
@ written again before anything reads them; then around a second Inside call that touches none of them and a loop of
@ such calls, all must be exact in the context at the call through psv_call (obs_reads_all: the whole context
@ compared with the interpreter's).

@ Reads r6 from the context (not an argument).
    .global cr_leaf_r6
    .type cr_leaf_r6, %function
    .thumb_func
cr_leaf_r6:
    add     r0, r0, r6
    bx      lr

@ Reads r4 and calls cr_leaf_r6.
    .global cr_mid_r4
    .type cr_mid_r4, %function
    .thumb_func
cr_mid_r4:
    push    {r4, lr}
    add     r0, r0, r4
    bl      cr_leaf_r6
    pop     {r4, pc}

@ Touches r0 only.
    .global cr_leaf_none
    .type cr_leaf_none, %function
    .thumb_func
cr_leaf_none:
    add     r0, r0, #5
    bx      lr

@ @test cr_caller r0=any r1=any
    .global cr_caller
    .type cr_caller, %function
    .thumb_func
cr_caller:
    push    {r4-r8, lr}
    add     r4, r0, #1
    eor     r5, r0, r1
    add     r6, r1, #7
    movs    r7, #0x41
    add     r8, r0, r1
    bl      cr_mid_r4
    cmp     r0, r1
    it      hi
    addhi   r7, r7, #2
    add     r0, r0, r5
    bl      cr_leaf_none
    add     r5, r5, r0
    movs    r2, #3
1:  bl      cr_leaf_none
    add     r8, r8, r0
    subs    r2, r2, #1
    bne     1b
    ldr     r3, =obs_reads_all
    blx     r3
    add     r0, r0, r5
    add     r0, r0, r6
    add     r0, r0, r7
    add     r0, r0, r8
    pop     {r4-r8, pc}
    .ltorg

@ Recursive Inside callees (context_reads, a fixed point over the module's calls): cr_rec reads r6 and calls itself;
@ cr_mut_a reads none of r4-r12 itself but calls cr_mut_b, which reads r7 and calls cr_mut_a. The caller's dirty r6
@ and r7 must be in the context at the calls that reach their reads (r7 defined only after cr_rec, so it is stored for
@ cr_mut_a's reach alone), r5 and r8 not (touched by neither, written again before the psv_call that reads them).
    .thumb
    .global cr_rec
    .type cr_rec, %function
    .thumb_func
cr_rec:
    push    {r4, lr}
    cmp     r0, #0
    beq     1f
    subs    r0, #1
    bl      cr_rec
    add     r0, r0, r6
1:  pop     {r4, pc}

    .global cr_mut_a
    .type cr_mut_a, %function
    .thumb_func
cr_mut_a:
    push    {r4, lr}
    cmp     r0, #0
    beq     1f
    subs    r0, #1
    bl      cr_mut_b
1:  pop     {r4, pc}

    .global cr_mut_b
    .type cr_mut_b, %function
    .thumb_func
cr_mut_b:
    push    {r4, lr}
    add     r1, r1, r7
    bl      cr_mut_a
    pop     {r4, pc}

@ @test cr_rec_caller r0=int:0:3 r1=any r2=any
    .global cr_rec_caller
    .type cr_rec_caller, %function
    .thumb_func
cr_rec_caller:
    push    {r4-r8, lr}
    ldr     r4, =obs_reads_all
    mov     r5, r2
    add     r6, r1, #5
    mov     r8, r0
    bl      cr_rec
    add     r7, r2, #9
    mov     r1, r0
    mov     r0, r8
    bl      cr_mut_a
    add     r0, r1, r5
    add     r0, r0, r8
    add     r5, r0, #1
    add     r6, r0, #2
    add     r7, r0, #3
    add     r8, r0, #4
    blx     r4
    pop     {r4-r8, pc}
    .ltorg
