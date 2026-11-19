#pragma once

// The host's floating-point environment while guest code runs. Generated code built with native floating point
// (arm64, PSVRECORE_GENERATED_CODE_NATIVE_FLOAT: guest.h, PSV_NATIVE_FLOAT) computes with the host's instructions and
// relies on the FPCR having the guest FPSCR's flush-to-zero (FZ) and default NaN (DN) bits; the rounding mode is
// always to nearest (an FPSCR write selecting another one traps). So a guest thread runs in "guest mode": its host
// thread's FPCR mirrors its FPSCR, set where guest code is entered from the runtime and kept through every import
// (HLE code runs in it too), except inside the imports of libraries registered as host_float, whose host code does
// arithmetic where FZ or DN would change results that matter: they run in the host's default environment
// (HostFloatScope). See ARCHITECTURE.md, "VFP registers".
//
// Where the mirror is set (guest mode entered or restored):
// - a guest thread's start (kernel/threads.cpp), and its end (back to the host's default);
// - call_guest (callbacks, fiber entries): the guest's on the way in, on the way out what the caller had (a guest
//   caller's mirror taken again from the context, which a vmsr in the callee may have changed);
// - sceFiber switches (fiber.cpp): the registers a fiber resumes with include its FPSCR;
// - the fiber thread backend: each guest fiber keeps its FPCR across the switches of its host thread;
// - vmsr (psv_vmsr in guest.h).
// Elsewhere (x64, arm64 without the option) everything here does nothing.

#include "psvrecore/recomp/guest.h"
#include "core/foundation/core/foundation.h"

namespace psvrecore::runtime {

#if defined(PSVRECORE_NATIVE_FLOAT) && defined(__aarch64__)
inline constexpr bool kNativeFloat = true;
#else
inline constexpr bool kNativeFloat = false;
#endif

// The calling host thread's floating-point state: its FPCR, and whether it is in guest mode.
struct FloatState {
    u64 fpcr = 0;
    bool guest = false;
};

FloatState float_state();
void set_float_state(const FloatState& s);
// Guest mode for the guest whose FPSCR the context holds; the host's default.
void enter_guest_float(const PsvContext* ctx);
void leave_guest_float();
// Whether the calling thread is in guest mode.
bool in_guest_float();

// The host's default floating-point environment for the scope (an import of a host_float library), and the guest's
// back after it (from the context, which guest code called inside may have changed). Nothing outside guest mode.
class HostFloatScope {
public:
    explicit HostFloatScope(const PsvContext* ctx) : ctx_(ctx) {
        if constexpr (kNativeFloat) {
            guest_ = in_guest_float();
            if (guest_) leave_guest_float();
        }
    }
    ~HostFloatScope() {
        if constexpr (kNativeFloat)
            if (guest_) enter_guest_float(ctx_);
    }
    HostFloatScope(const HostFloatScope&) = delete;
    HostFloatScope& operator=(const HostFloatScope&) = delete;

private:
    const PsvContext* ctx_;
    bool guest_ = false;
};

}  // namespace psvrecore::runtime
