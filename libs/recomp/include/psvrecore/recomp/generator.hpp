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
#include "psvrecore/analysis/vfp.hpp"
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
    // The floating-point registers and the FPSCR (analysis/vfp.hpp): in the context everywhere, translated as before
    // they were locals (psvrecomp --vfp-in-context); every call taken to read and write all of them, no callee's
    // summary used (psvrecomp --vfp-calls-conservative).
    bool vfp_in_context = false;
    bool vfp_calls_conservative = false;
    // Advanced SIMD in the context form everywhere, guest_simd.h's helpers with the locals synced around each
    // instruction, as before the registers' values were translated (psvrecomp --neon-in-context); no Q register held
    // as one vector local (psvrecomp --no-q-locals).
    bool neon_in_context = false;
    bool no_q_locals = false;
    // Internal entries (analysis/registers.hpp, EntryKind): direct calls between the module's functions pass r0-r3, sp
    // and lr as C arguments of fi_<address> and get r0/r1 back by value; off, every call through f_<address> as before
    // (psvrecomp --no-internal-calls). Functions given none, so that every direct call goes through f_<address> (a
    // game's stand-in for it, task 8's replacements: psvrecomp --no-inline).
    bool internal_calls = true;
    nx::vector<u32> no_internal;
    // Indirect calls through the module's table (guest.h, PsvDispatch, psv_call_in), Thumb functions of the module found
    // there without a call into the runtime; off, all through psv_call (psvrecomp --no-call-table).
    bool call_table = true;
    // With call_table: each indirect call site looks in the table itself (inline: psv_call_in at the site, a larger
    // program) or calls the module's one function that does (<name>_call, the default; psvrecomp --call-table-inline).
    bool call_table_inline = false;
    // Internal entries of at most this many instructions and no jump table are inlined: static in each .c file whose
    // functions call them (psvrecomp --inline-limit; 0: none).
    u32 inline_limit = 8;
    // The internal entries not inlined are marked noinline (the C compiler would otherwise copy them into the callers
    // in their own file and their wrapper, beside the out-of-line copy; psvrecomp --inline-in-file lets it).
    bool noinline_internal = true;
    // Host implementations of guest functions (psvrecomp --replace, --replace-table; ARCHITECTURE.md, "Host
    // implementations of guest functions"). Every direct call, tail branch, table-branch case and run-into of a replaced
    // function calls `symbol` (void symbol(PsvContext*, uint32_t ret)), taken to have the AAPCS's contract (analysis::
    // RegisterOptions::hosts): every dirty register in the context before it, r4-r11 and sp kept in locals across it,
    // r0-r3, r12 and lr read back after it where live; the flags and the floating-point registers as for a call of its
    // code. It has no internal entry and is never inlined, and its f_<address> (the function table's entry: indirect
    // calls, jumps through the runtime, other modules' imports of it) calls `symbol` too. A function whose code reads
    // flags at entry is not replaced (a note in Stats::replacement_notes). With a
    // `guard` (int guard(const PsvContext*)), f_<address> calls `symbol` only when the guard holds for the arguments,
    // else the guest code's translation, kept as fo_<address>: for functions whose result on some inputs depends on the
    // guest's algorithm (memcpy of overlapping buffers).
    struct Replacement {
        u32 code = 0;  // a function code discovery found (bit 0: Thumb)
        nx::string symbol;
        nx::string guard;
    };
    nx::vector<Replacement> replacements;
};

// The host functions and guards the runtime defines (guest.h, psv_host_*; libs/recomp/src/host_functions.cpp): the
// names psvrecomp accepts with that prefix (a test checks the list against guest.h's declarations).
struct RuntimeHostFunction {
    const char* name;
    bool guard;  // int name(const PsvContext*), else void name(PsvContext*, uint32_t ret)
};
inline constexpr RuntimeHostFunction kRuntimeHostFunctions[] = {
    {"psv_host_memcpy", false},          {"psv_host_memmove", false},           {"psv_host_memset", false},
    {"psv_host_memcmp", false},          {"psv_host_strlen", false},            {"psv_host_strcmp", false},
    {"psv_host_strncpy", false},         {"psv_host_wcslen", false},            {"psv_host_atomic_fetch_add_4", false},
    {"psv_host_atomic_fetch_sub_4", false}, {"psv_host_memcpy_exact", true},    {"psv_host_atomic_order_valid", true},
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
    // Floating-point registers in locals (analysis/vfp.hpp): functions with such locals; locals read from the context
    // at entry; places (calls, Advanced SIMD instructions, exits) with locals written to the context before them, and
    // with locals read back after them (calls, Advanced SIMD); traps that write them; calls after which a live local
    // stays (the callee does not write it); Advanced SIMD instructions with locals written or read around them; D
    // registers held as two floats, and as a double (counted per function); vcmp + vmrs APSR_nzcv pairs computed as one
    // compare, and the conditions after them that compare its operands.
    usize vfp_functions = 0;
    usize vfp_entry_loads = 0;
    usize vfp_stores = 0;
    usize vfp_call_loads = 0;
    usize vfp_trap_stores = 0;
    usize vfp_calls_keeping = 0;
    usize vfp_simd_syncs = 0;
    usize vfp_s_held = 0;
    usize vfp_d_held = 0;
    usize vfp_q_held = 0;      // Q registers held as one vector local
    // Advanced SIMD: instructions translated on the registers' values, and in the context form (no translation on
    // values; with --neon-in-context all of them)
    usize neon_values = 0;
    usize neon_context = 0;
    usize vfp_fused_compares = 0;
    usize vfp_fused_conditions = 0;
    // Instructions translated to a trap, by mnemonic (undefined, unimplemented, privileged...).
    nx::map<nx::string, usize> traps;
    // Internal entries: functions with one that reaches nothing outside (Inside) and that may (Flushed); functions
    // without one, by why (analysis::EntryReason), and Flushed ones by why; direct calls to Inside entries, to Flushed
    // ones, through f_<address> (by why the callee has no internal entry); returns of internal entries (no compare with
    // ret); their C arguments and results.
    usize internal_inside = 0;
    usize internal_flushed = 0;
    nx::map<nx::string, usize> public_reasons;
    nx::map<nx::string, usize> flushed_reasons;
    usize calls_inside = 0;
    usize calls_flushed = 0;
    usize calls_public = 0;
    nx::map<nx::string, usize> calls_public_reasons;
    nx::map<nx::string, usize> calls_flushed_reasons;
    usize internal_returns = 0;
    // Inlining: functions inlined, the direct calls of them, the static copies the .c files hold.
    usize functions_inlined = 0;
    usize calls_inlined = 0;
    usize inline_copies = 0;
    usize internal_params = 0;
    usize internal_results = 0;
    // Host implementations (Options::replacements): functions replaced, of them with a guard; calls of them (direct
    // calls, and tail branches, table-branch cases and run-intos); the direct calls after which the caller reads a
    // register or flag the AAPCS lets a callee change (r1-r3, r12, lr, NZCV) before writing it: such a caller relies on
    // what the guest's own code leaves there, which the host implementation does not (each is listed in
    // replacement_notes, with the replacements the generator could not make).
    usize replaced = 0;
    usize replaced_guarded = 0;
    usize replaced_calls = 0;
    usize replaced_tails = 0;
    usize replaced_relying = 0;
    nx::vector<nx::string> replacement_notes;
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
