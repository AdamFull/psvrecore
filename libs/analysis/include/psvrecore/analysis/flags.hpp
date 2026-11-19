#pragma once

// The condition flags N, Z, C and V: what each instruction reads and writes, and where each one is live, over each
// function's control-flow graph (cfg.hpp) and across the module's calls. The code generator computes a flag only
// where it is live, keeps the flags in locals, and puts them in the context only where code it does not see may
// read them. See ARCHITECTURE.md, "Condition flags", for the contract at calls, returns and module boundaries.
//
// The four flags are independent: each instruction's effect on one does not depend on the others, so every set below
// is four separate liveness problems solved at once. Q and GE are not here: generated code keeps them in the context.
//
// Per function, liveness is solved twice: with its returns reading no flag and with its returns reading all four. At
// any point the flags live when the returns read the set R are (live with none) | (live with all & R) (per flag: R
// only matters for that flag). That gives each function's summary without knowing its callers:
// - need: the flags it reads at entry, on some path, before writing them (itself or what it calls);
// - pass: the flags live at entry when its returns read all four: `need`, and those that reach a return unwritten on
//   some path. A caller that reads flag X after the call puts X in the context first when X is in `pass`.
// and then, from its callers, `live_out`: the flags its returns must leave in the context.

#include "psvrecore/analysis/cfg.hpp"
#include "psvrecore/analysis/program.hpp"
#include "core/foundation/containers/map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::analysis {

using FlagSet = u8;  // bit 3 N, 2 Z, 1 C, 0 V (APSR bits 31-28)
inline constexpr FlagSet kFlagV = 1, kFlagC = 2, kFlagZ = 4, kFlagN = 8, kNoFlags = 0, kAllFlags = 15;

// "nzcv" with the absent ones as '-'.
nx::string flag_names(FlagSet f);
// The flags a condition code tests.
FlagSet condition_flags(arm::Cond c);

struct FlagEffect {
    FlagSet uses = 0;   // read (its condition included)
    FlagSet defs = 0;   // written when it executes (computed when live after it)
    // Of those, the ones whose old value is gone after it executes. A shift by a register (lsls r0, r1, r2; ands r0,
    // r1, r2, lsl r3) writes the carry but keeps it for a shift by 0: it neither ends the old carry's life nor reads
    // it unless the carry is live after it. (The writes of a conditional instruction end nothing either: the liveness
    // keeps the path where it does not execute.)
    FlagSet kills = 0;
};
// The instruction's own reads and writes, as the code generator translates it (not those of a call it makes).
FlagEffect flag_effect(const arm::Instruction& in);

struct FlagOptions {
    // No liveness: every flag live everywhere, in the context at every call, jump and return, and read back after
    // each call (the generator before flag liveness; psvrecomp --flags-everywhere).
    bool everywhere = false;
    // Flags written to the context before every call or jump to code the module does not know (indirect calls and
    // jumps, imports, svc), beyond what the module's own functions read at entry: what other modules' functions read
    // at entry (psvrecomp --flags-given-at-entry).
    FlagSet given_at_entry = 0;
    // Flags every function's return leaves in the context, beyond what the module's own indirect calls read after
    // they return: what other modules' indirect calls read (psvrecomp --flags-given-at-return).
    FlagSet given_at_return = 0;
    // Functions whose returns leave all four flags in the context. Exported functions are added (other modules
    // call them through their imports, and compiler helpers such as __aeabi_cdcmple return their result in the
    // flags).
    nx::vector<u32> full_exit;
};

struct FunctionFlags {
    FlagSet need = 0;      // read at entry before being written, on some path (see above)
    FlagSet pass = 0;      // live at entry when the returns read all four (see above; includes need)
    FlagSet live_out = 0;  // left in the context at every return
    FlagSet live_in = 0;   // read from the context at entry (need, pass & live_out, and what a trap may show)
    // By instruction index (Function::code):
    nx::vector<FlagSet> after;        // live after the instruction's own effect: the flags it must compute
    nx::vector<FlagSet> after_call;   // a call: live when it returns (read back from the context)
    nx::vector<FlagSet> before_call;  // a call: written to the context before it
    nx::vector<FlagSet> live_before;  // live before the instruction (its condition included; what a trap may show)
};

struct FlagLiveness {
    bool everywhere = false;
    nx::map<u32, FunctionFlags> functions;  // by code address
    // The module's contract (recorded in its PsvModuleCode, checked when modules are registered):
    FlagSet needed_at_entry = 0;   // some function reads them at entry (any function may be entered from outside)
    FlagSet given_at_entry = 0;    // in the context at every call or jump to code the module does not know
    FlagSet needed_at_return = 0;  // read after an indirect call or jump (to code the module does not know) returns
    FlagSet given_at_return = 0;   // in the context at every function's return

    const FunctionFlags* at(u32 code) const {
        auto it = functions.find(code);
        return it == functions.end() ? nullptr : &it->second;
    }
    // The flags to put in the context before control enters the function at `code` (a call or a tail call), when
    // `after` are live where it returns to.
    FlagSet into(u32 code, FlagSet after) const;
    // The same for code the module does not know (an import, an indirect call or jump, svc).
    FlagSet into_unknown(FlagSet after) const { return everywhere ? kAllFlags : FlagSet(given_at_entry | after); }
    // The flags to put in the context at an edge out of `fn` (none for Next and Trap edges).
    FlagSet at_exit(const FunctionFlags& fn, const Edge& e) const;
};

FlagLiveness flag_liveness(const Program& program, const FlagOptions& options = {});

}  // namespace psvrecore::analysis
