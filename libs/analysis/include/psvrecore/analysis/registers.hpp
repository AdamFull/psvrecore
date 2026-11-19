#pragma once

// The core registers r0-r14 in the generated code's locals: what each instruction reads and writes, where each
// register's local differs from the context, where it is live, and which registers each function gives back to its
// caller as it got them. The code generator keeps the registers in locals of each function, puts them in the context
// only where code outside the function may read them, and reads them back from it only where code outside may have
// written them. See ARCHITECTURE.md, "Registers", for the contract at calls, returns and the other boundaries.
//
// Per function, over its control-flow graph (cfg.hpp):
// - a forward pass gives each register two symbolic values at every point: the guest's (what the local holds) and
//   the context's. A symbolic value is unknown, the value a register had at the function's entry, the entry's stack
//   pointer plus a constant, or the value the context held after a given call (or a store before it). The stack
//   words the function writes at known offsets from the entry's stack pointer are tracked as well (push, str, stm,
//   vpush...). A register whose two values are not the same symbol is dirty: written to the context at the next
//   place code outside the function may read it.
// - a backward pass gives the registers live at every point: read by an instruction, or written to the context at a
//   call, an exit or a trap, before being written. A function reads at entry the registers live there; after a call
//   it reads back the live registers the callee may have changed.
//
// Callee-saved registers. A function preserves r4-r11 and sp that are, at every return when it was called with LR
// holding the address it returns to (a direct call: bl), the values they had at entry: from its own code (a
// register pushed at entry and popped before the return; sp moved back), and through its calls, where a callee
// preserves what its own analysis says. The greatest fixed point over the module's calls and tail calls (a function
// that only calls itself keeps what it keeps on the paths that return). A return through anything but the LR it was
// given, an indirect jump, an import's tail call: it preserves nothing. A caller keeps a preserved register in its
// local across a direct call.
//
// Code outside the module's view (imports, indirect calls, svc) is taken to return with the stack pointer it was
// called with (AAPCS); a function whose analysis relies on that, and that has direct callers, checks it after every
// such call (the generated code traps if not; RegisterOptions::check_sp). Everything else such code may change. One
// thing is assumed and not checked (except by the generated code built with PSV_CHECK_KEPT_REGISTERS): stack words
// a function writes at known offsets of its frame are changed only by its own stores there (not by its callees, not
// by a store through another pointer). See KNOWN_ISSUES.md. The proof is cross-checked against the module's
// exception index table (unwind_restored).

#include "psvrecore/analysis/cfg.hpp"
#include "psvrecore/analysis/program.hpp"
#include "core/foundation/containers/map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::analysis {

using RegSet = u16;  // bit k: register rk, k = 0..14 (13: sp, 14: lr); the PC is not one of them
inline constexpr RegSet kNoRegisters = 0, kAllRegisters = 0x7FFF, kCalleeSaved = 0x0FF0, kSpRegister = 1u << 13,
                        kLrRegister = 1u << 14, kPreservable = kCalleeSaved | kSpRegister;

// "r0 r4-r7 sp lr", or "none".
nx::string register_names(RegSet r);

struct RegisterEffect {
    RegSet uses = 0;     // read by the instruction's own effect (not the call it makes)
    RegSet defs = 0;     // written by it when it executes, every one of them
    bool traps = false;  // its translation may stop inside it (a trap before its writes are done)
};
// The instruction's reads and writes as the code generator translates it.
RegisterEffect register_effect(const arm::Instruction& in);

// Where a caller keeps the registers a callee preserves in its locals across a direct call, rather than reading them
// back from the context (where they are the same, known by the analysis either way): keeping one costs the host
// function a register (saved in its prologue) or a spill, reading it back a load after each call.
enum class KeepPolicy : u8 {
    All,    // at every direct call
    Loops,  // at direct calls inside a loop of the function (each iteration saves the loads)
    None,   // nowhere (only sp's check after calls to code outside the module's view keeps sp)
};

struct RegisterOptions {
    // Callee-saved knowledge: what each function preserves (off: psvrecomp --no-callee-saved; every register live
    // after a call is read back, and nothing is known to stay the same across one).
    bool callee_saved = true;
    KeepPolicy keep = KeepPolicy::All;
    // Code outside the module's view (imports, indirect calls, svc) is taken to return with the sp it was called with;
    // checked after each such call in the functions whose callers rely on it (the generated code traps if not). Off:
    // not checked (an assumption, KNOWN_ISSUES), and sp read back after such calls (psvrecomp --no-sp-check).
    bool check_sp = true;
    // The instructions' effects by function (code address) and instruction index, instead of register_effect: the
    // code generator gives what its translation of each instruction reads and writes.
    const nx::map<u32, nx::vector<RegisterEffect>>* effects = nullptr;
};

struct FunctionRegisters {
    RegSet load_at_entry = 0;  // read from the context at entry
    RegSet preserved = 0;      // of r4-r11 and sp: the same at its returns as at entry (when entered by a direct call)
    RegSet touched = 0;        // every register it reads, writes, stores or loads (the locals it has)
    bool checks_sp = false;    // checks the stack pointer after its calls to code outside the module's view
    // Every way out of it goes back to the address LR held at entry, its return address when called directly: its
    // returns branch to that LR, its tail calls pass it on (to functions that may not, themselves); no jump through
    // the runtime, no import's tail call. (What the floating-point registers' summaries rely on: analysis/vfp.hpp.)
    bool returns_to_lr = false;
    // By instruction index (Function::code):
    nx::vector<RegSet> trap_store;   // dirty before the instruction: written to the context by a trap inside it
                                     // (with what the instruction has written so far)
    nx::vector<RegSet> call_store;   // a call: written to the context before it
    nx::vector<RegSet> call_load;    // a call: read back after it
    nx::vector<RegSet> call_kept;    // a call: live after it, kept in locals (the callee preserves them)
    nx::vector<RegSet> exit_store;   // written to the context before an exit inside its translation (a return, a
                                     // jump through the runtime, a tail call)
    nx::vector<RegSet> after_store;  // written before the exit after its translation (running into another function,
                                     // a place discovery did not follow)
    nx::vector<RegSet> live_before;  // live before the instruction (tests, statistics)
};

// The cross-check of what functions preserve against the module's exception index table (.ARM.exidx): the core
// registers each entry's unwind opcodes restore (r4-r11, lr; ARM EHABI, the compact models and the generic one).
// A function the analysis finds to preserve a register it writes should restore it when unwound; the counts:
struct UnwindCheck {
    usize entries = 0;      // functions with an exception index entry whose opcodes decode
    usize undecoded = 0;    // entries that cannot unwind, describe no frame (only "finish": leaves), or do not decode
    usize agree = 0;        // functions whose every preserved register it writes the unwinder restores
    usize disagree = 0;     // preserved and written, not restored by the unwinder (the proof and the table differ)
    usize unproven = 0;     // the unwinder restores a register of r4-r11 the analysis does not show preserved
    nx::vector<u32> disagreeing;  // (the first few, code addresses)
};

// The registers each exception index entry's unwind opcodes restore, by function address (bit 0 clear); entries
// that cannot unwind or do not decode are left out (counted in `undecoded`).
nx::map<u32, RegSet> unwind_restored(const Image& image, usize* undecoded = nullptr);

struct RegisterAnalysis {
    bool callee_saved = true;
    UnwindCheck unwind;
    nx::map<u32, FunctionRegisters> functions;  // by code address
    const FunctionRegisters* at(u32 code) const {
        auto it = functions.find(code);
        return it == functions.end() ? nullptr : &it->second;
    }
};

RegisterAnalysis register_analysis(const Program& program, const RegisterOptions& options = {});

}  // namespace psvrecore::analysis
