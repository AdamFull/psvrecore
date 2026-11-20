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

// Adds a module's functions. Its imports are not resolved here (the loader does that). Its condition-flag contract
// (guest.h, PsvModuleCode) is checked against every module registered: what each reads at entry and after indirect
// calls, every other must provide.
PSVRECORE_API void register_module(const PsvModuleCode& module);
// The modules registered whose flag contracts do not fit (one line each, with the psvrecomp option that fixes it);
// empty when they all do. The loader refuses to load a program then.
PSVRECORE_API nx::string flag_contract_problems();
PSVRECORE_API void clear_modules();
PSVRECORE_API PsvFunction find_function(u32 code);
// The import whose stub is at `code` (as the module's import_stubs give it), or nullptr. Calls and jumps through a
// register to a stub (fixed-address code may build a stub's address with movw/movt) call the import, as the stub
// would on the console.
PSVRECORE_API const PsvImport* find_import_stub(u32 code);
// Puts `f` where the function at `code` was (its address as calls give it: bit 0 set for Thumb code), for the calls
// that look it up (indirect calls and jumps: direct calls
// are compiled to the function itself), and returns the one it replaces (nullptr: none was there). Before the
// game starts: the registry is not locked.
PSVRECORE_API PsvFunction override_function(u32 code, PsvFunction f);
// The memory the registered modules' indirect-call tables hold (guest.h, PsvDispatch), in bytes.
PSVRECORE_API usize call_table_bytes();

struct TrapInfo {
    u32 pc = 0;
    u32 kind = 0;
    u32 target = 0;          // PSV_TRAP_BAD_JUMP from psv_jump/psv_call: the address nothing is known at
    const PsvImport* import = nullptr;  // an import nothing implements
};
PSVRECORE_API const char* trap_name(u32 kind);
PSVRECORE_API nx::string describe(const TrapInfo& trap);

using TrapHandler = void (*)(PsvContext* ctx, const TrapInfo& trap);
// Returns the previous handler. nullptr restores the default (report and abort).
PSVRECORE_API TrapHandler set_trap_handler(TrapHandler handler);

// The kinds psv_trap is given for failures found by the services themselves.
inline constexpr u32 kTrapUnknownTarget = 0x100;  // psv_call/psv_jump to an address no function starts at
inline constexpr u32 kTrapMissingImport = 0x101;  // an import the loader resolved to nothing
inline constexpr u32 kTrapSvc = 0x102;            // svc: user code has no system calls of its own

}  // namespace psvrecore::recomp
