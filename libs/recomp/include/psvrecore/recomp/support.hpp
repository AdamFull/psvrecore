#pragma once

// The services guest.h declares, for programs that run recompiled code: the registry of modules' functions (for
// indirect calls and jumps), and what happens on a trap.
//
// The registry maps code addresses to host functions. Traps go to a handler: by default it reports the trap and
// ends the program; a test can install one that unwinds instead (it must not return).

#include "psvrecore/recomp/guest.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::recomp {

// Adds a module's functions. Its imports are not resolved here (the loader does that).
void register_module(const PsvModuleCode& module);
void clear_modules();
PsvFunction find_function(u32 code);

struct TrapInfo {
    u32 pc = 0;
    u32 kind = 0;
    u32 target = 0;          // PSV_TRAP_BAD_JUMP from psv_jump/psv_call: the address nothing is known at
    const PsvImport* import = nullptr;  // an import nothing implements
};
const char* trap_name(u32 kind);
nx::string describe(const TrapInfo& trap);

using TrapHandler = void (*)(PsvContext* ctx, const TrapInfo& trap);
// Returns the previous handler. nullptr restores the default (report and abort).
TrapHandler set_trap_handler(TrapHandler handler);

// The kinds psv_trap is given for failures found by the services themselves.
inline constexpr u32 kTrapUnknownTarget = 0x100;  // psv_call/psv_jump to an address no function starts at
inline constexpr u32 kTrapMissingImport = 0x101;  // an import the loader resolved to nothing
inline constexpr u32 kTrapSvc = 0x102;            // svc: user code has no system calls of its own

}  // namespace psvrecore::recomp
