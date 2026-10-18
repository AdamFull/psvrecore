/* Guest code for the runtime tests, compiled as generated code is (C, with the generated-code options): a thread
 * entry that ends its thread from inside a nested call, through an import, as recompiled code would. The kernel
 * ends the thread by unwinding through these C frames. */

#include "psvrecore/recomp/guest.h"

PsvImport test_exit_import = {0x5E8D0E22u, 0x0C8A38E1u, "SceThreadmgrCoredumpTime", 0};  /* sceKernelExitThread */
volatile int test_returned_after_exit = 0;

static void nested(PsvContext* restrict ctx, uint32_t ret) {
    ctx->r[0] = 7;
    ctx->r[14] = ret;
    psv_call_import(ctx, &test_exit_import, ret);
    test_returned_after_exit = 1; /* (never reached) */
}

void test_exit_entry(PsvContext* restrict ctx, uint32_t ret) {
    nested(ctx, 0x82100001u);
    test_returned_after_exit = 2;
    ctx->r[0] = 99;
    (void)ret;
}
