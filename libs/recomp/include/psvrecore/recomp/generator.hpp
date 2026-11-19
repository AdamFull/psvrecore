#pragma once

// The code generator: a module's functions, as code discovery found them, translated to C that includes guest.h.
//
// Output, for a module named N:
// - N_functions.h: the prototype of every function;
// - N_code_<k>.c: the functions, a few thousand instructions per file so they compile in parallel;
// - N_module.c: the module's PsvModuleCode (functions by address, imports and their stubs).
//
// An instruction the generator does not translate becomes a trap at that place, and is counted in the statistics;
// nothing is skipped silently.

#include "psvrecore/analysis/flags.hpp"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/analysis/registers.hpp"
#include "core/foundation/containers/map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::recomp {

struct Options {
    nx::string name;                    // C identifier prefix of the module's tables (libc, eboot...)
    bool comments = true;               // each instruction's disassembly as a comment
    u32 instructions_per_file = 20000;  // how much code each .c file gets (psvrecomp --instructions-per-file)
    // The condition flags (analysis/flags.hpp, FlagOptions): no liveness (psvrecomp --flags-everywhere); what other
    // modules' code reads at entry and after their indirect calls (psvrecomp --flags-given-at-entry,
    // --flags-given-at-return); functions whose returns leave all four (tests: functions called as the runtime calls
    // them, whose every flag at return is compared).
    bool flags_everywhere = false;
    u8 flags_given_at_entry = 0;
    u8 flags_given_at_return = 0;
    nx::vector<u32> full_exit_flags;
    // The core registers (analysis/registers.hpp): in the context everywhere, as before they were locals
    // (psvrecomp --registers-in-context); no callee-saved knowledge, every register live after a call read back
    // from the context (psvrecomp --no-callee-saved).
    bool registers_in_context = false;
    bool no_callee_saved = false;
    // Where callers keep a callee's preserved registers in locals rather than read them back (psvrecomp
    // --keep-registers: all, loops, none; analysis::KeepPolicy).
    analysis::KeepPolicy keep_registers = analysis::KeepPolicy::All;
    // The stack pointer checked after calls to code outside the module's view where callers rely on it (psvrecomp
    // --no-sp-check: assumed, not checked; analysis::RegisterOptions::check_sp).
    bool check_sp = true;
};

struct GeneratedFile {
    nx::string name;
    nx::string text;
};

struct Stats {
    usize functions = 0;
    usize instructions = 0;
    usize tail_calls = 0;
    usize import_calls = 0;
    usize indirect_calls = 0;
    usize indirect_jumps = 0;
    usize jump_tables = 0;
    usize literals_folded = 0;
    // Condition flags: instructions that write a flag, and those of them that compute one (one they write is live);
    // calls (to functions, imports, through the runtime, svc), and those with flags written to the context before
    // them / read back after; returns (branches through a register or memory that are not table branches), and those
    // with flags written to the context; functions that read a flag at entry before writing it, and functions whose
    // returns leave flags for their callers.
    usize flag_setters = 0;
    usize flag_setters_live = 0;
    usize call_sites = 0;
    usize call_sites_store = 0;
    usize call_sites_load = 0;
    usize return_sites = 0;
    usize return_sites_store = 0;
    usize functions_need = 0;
    usize functions_live_out = 0;
    // Core registers in locals: read from the context at functions' entries; written to it (before calls, exits,
    // traps); read back after calls; direct calls with registers kept in locals across them, and those registers;
    // calls to code outside the module's view followed by a check of the stack pointer; functions that preserve each
    // of r4-r11 and sp (by register number), all of them, and that check the stack pointer.
    usize register_entry_loads = 0;
    usize register_stores = 0;
    usize register_trap_stores = 0;
    usize register_call_loads = 0;
    usize calls_keeping = 0;
    usize registers_kept = 0;
    usize stack_checks = 0;
    usize functions_preserving[15] = {};
    usize functions_preserving_all = 0;
    usize functions_checking_sp = 0;
    // Instructions whose translation reads or writes other registers than analysis::register_effect says (the
    // analysis uses the translation's; a mismatch is a generator or model bug to look at), and the first few.
    usize register_effect_mismatches = 0;
    nx::vector<nx::string> register_notes;
    // Instructions translated to a trap, by mnemonic (undefined, unimplemented, privileged...).
    nx::map<nx::string, usize> traps;
};

struct Output {
    nx::vector<GeneratedFile> files;
    Stats stats;
    // Each function's flag summary and the module's contract (analysis::FlagLiveness without its per-instruction sets).
    analysis::FlagLiveness flags;
    // Each function's register summary (analysis::RegisterAnalysis without its per-instruction sets).
    analysis::RegisterAnalysis registers;
};

Output generate(const analysis::Program& program, const Options& options);

// The C name of the function at a code address.
nx::string function_name(u32 code);

}  // namespace psvrecore::recomp
