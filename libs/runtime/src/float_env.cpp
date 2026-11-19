#include "psvrecore/runtime/float_env.hpp"

namespace psvrecore::runtime {

namespace {

// (per host thread; the fiber backend carries it with each guest fiber across its host threads: backend_fibers.cpp)
thread_local bool g_guest = false;

u64 read_fpcr() {
#if defined(__aarch64__)
    u64 fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    return fpcr;
#else
    return 0;
#endif
}

void write_fpcr(u64 fpcr) {
#if defined(__aarch64__)
    if (read_fpcr() != fpcr) __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#else
    (void)fpcr;
#endif
}

}  // namespace

FloatState float_state() {
    if constexpr (!kNativeFloat) return {};
    return {read_fpcr(), g_guest};
}

void set_float_state(const FloatState& s) {
    if constexpr (!kNativeFloat) return;
    write_fpcr(s.fpcr);
    g_guest = s.guest;
}

void enter_guest_float(const PsvContext* ctx) {
    if constexpr (!kNativeFloat) return;
    psv_native_set_fpcr(ctx ? ctx->fpscr : 0u);
    g_guest = true;
}

void leave_guest_float() {
    if constexpr (!kNativeFloat) return;
    psv_native_set_fpcr(0);
    g_guest = false;
}

bool in_guest_float() { return kNativeFloat && g_guest; }

}  // namespace psvrecore::runtime
