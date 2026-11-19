#pragma once

// The floating-point registers (d0-d31; s0-s31 are the halves of d0-d15) and the FPSCR in the generated code's locals:
// how each function holds them, where each local differs from the context, where it is live, and what each function
// may read and write of them in the context (its callers keep the rest in their locals across a call to it). See
// ARCHITECTURE.md, "VFP registers", for the contract at calls, returns and the other boundaries.
//
// Units. The register file is 64 words (word k: vfp.s[k]; d<n> is words 2n and 2n+1). Each function holds d<n>
// (n < 16) either as two float locals s<2n>, s<2n+1> or as one double local d<n>, by how its instructions access it;
// d16-d31 are always doubles. A unit is one such local: an S register, or a whole D register. An access to part of a
// unit (an S register of a D-held register, a byte lane) reads the whole unit. The FPSCR is two units: its NZCV
// (bits 31-28, what vcmp writes: the local `fpnzcv`) and the rest (FZ, DN, QC, the cumulative flags: `fpscr`).
//
// Per function, over its control-flow graph (cfg.hpp):
// - a forward pass gives the units that may be dirty (written since the context last held them) at every point;
// - a backward pass gives the live units: read by an instruction, or written to the context from their local at a
//   call, an exit or a trap, before being written. A function reads at entry the units live there.
// The places where code outside the function reads or writes the context: calls (direct, imports, through the
// runtime, svc), Advanced SIMD instructions (their helpers work on the context's registers), exits (returns, jumps
// through the runtime, tail calls, running into another function) and traps. Before each, the dirty units it may
// read are written to the context; after a call or an Advanced SIMD instruction, the live units it may have written
// are read back.
//
// What a call may read and write: a direct call to a function of the module reads and writes what that function's
// summary says (`reads`, `writes`: the words and FPSCR parts its own code touches, and its callees', the least fixed
// point over the module's calls); everything when its returns are not shown to go back to the return address it was
// given (analysis/registers.hpp, FunctionRegisters::returns_to_lr: else a return is a jump through the runtime to code
// that may do anything), when it calls code outside the module's view or jumps through the runtime. Calls to imports,
// through the runtime and svc read and write everything. Nothing is assumed of the calling convention (AAPCS's
// callee-saved d8-d15 are kept across a call only when the callee is seen not to write them).

#include <bit>

#include "psvrecore/analysis/cfg.hpp"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/analysis/registers.hpp"
#include "core/foundation/containers/map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::analysis {

inline constexpr u8 kFpscrNzcv = 1, kFpscrRest = 2, kFpscrAll = 3;

// Words of the register file (bit k: vfp.s[k]) and parts of the FPSCR (kFpscrNzcv, kFpscrRest).
struct VfpSet {
    u64 words = 0;
    u8 fpscr = 0;

    constexpr bool empty() const { return words == 0 && fpscr == 0; }
    constexpr bool operator==(const VfpSet&) const = default;
    constexpr VfpSet operator|(VfpSet o) const { return {words | o.words, static_cast<u8>(fpscr | o.fpscr)}; }
    constexpr VfpSet operator&(VfpSet o) const { return {words & o.words, static_cast<u8>(fpscr & o.fpscr)}; }
    constexpr VfpSet minus(VfpSet o) const { return {words & ~o.words, static_cast<u8>(fpscr & ~o.fpscr)}; }
    VfpSet& operator|=(VfpSet o) { return *this = *this | o; }
};
inline constexpr VfpSet kNoVfp{}, kAllVfp{~0ull, kFpscrAll};

constexpr u64 d_words(u32 n) { return 3ull << (2 * n); }
// The words of D registers any of whose words are in `w` (both halves of each).
constexpr u64 whole_d(u64 w) { return w | ((w & 0x5555555555555555ull) << 1) | ((w >> 1) & 0x5555555555555555ull); }

// "s0-s3 d8 d16-d17 fpscr", or "none" (by words: a D register when both its words are in, else its S registers).
nx::string vfp_names(VfpSet s);

// What an instruction's translation reads and writes of the floating-point registers, recorded by the code
// generator's first translation of the function (as RegisterEffect for the core registers).
struct VfpEffect {
    VfpSet uses;         // read through the locals (a word written in part counts as read)
    VfpSet defs;         // written through the locals, whole words
    u32 s_access = 0;    // d0-d15 accessed a word at a time (an S register, a 32-bit scalar, a lane): bit n
    u32 d_access = 0;    // D registers accessed whole: bit n (n < 32)
    VfpSet context;      // read and written in the context instead (Advanced SIMD): there before it, read back after
    bool traps = false;  // its translation may stop inside it
};

struct VfpOptions {
    // What direct calls may read and write from the callees' summaries; off: every call reads and writes everything
    // (psvrecomp --vfp-calls-conservative).
    bool summaries = true;
    // Each function's instructions' effects (by code address, instruction index): the code generator's.
    const nx::map<u32, nx::vector<VfpEffect>>* effects = nullptr;
    // FunctionRegisters::returns_to_lr: which functions return to the address they were called with. Without it no
    // function's summary is known (every call reads and writes everything).
    const RegisterAnalysis* registers = nullptr;
};

struct FunctionVfp {
    u32 s_view = 0;        // bit n (n < 16): d<n> is held as two float locals; else (and d16-d31) as a double
    VfpSet touched;        // the units it has locals for (words; both of a D-held register)
    VfpSet load_at_entry;  // read from the context at entry
    // What it may read and write of the context, its callees included (words of whole D registers; kAllVfp when
    // nothing is known): what its direct callers put in the context before calling it, and read back after.
    VfpSet reads, writes;
    // By instruction index (Function::code), in units (words; both of a D-held register):
    nx::vector<VfpSet> trap_store;   // dirty before the instruction: written to the context by a trap inside it
    nx::vector<VfpSet> call_store;   // a call or Advanced SIMD instruction: written to the context before it
    nx::vector<VfpSet> call_load;    // a call or Advanced SIMD instruction: read back after it
    nx::vector<VfpSet> exit_store;   // written before an exit inside its translation (a return, a jump, a tail call)
    nx::vector<VfpSet> after_store;  // written before the exit after its translation (running into another function,
                                     // a place discovery did not follow)
    nx::vector<VfpSet> live_before;  // live before the instruction
    nx::vector<u8> may_trap;         // its translation may trap (VfpEffect::traps)
    usize calls_keeping = 0;         // calls after which a live unit stays in its local (the callee does not write it)

    // The units of the words in `s`: both words of a D-held register any of whose words is in.
    VfpSet units(VfpSet s) const { return {s.words | whole_d(s.words & ~s_words()), s.fpscr}; }
    // The words held as floats.
    u64 s_words() const {
        u64 w = 0;
        for (u32 v = s_view; v; v &= v - 1) w |= d_words(static_cast<u32>(std::countr_zero(v)));
        return w;
    }
    bool s_held(u32 n) const { return n < 16 && (s_view & (1u << n)); }
};

struct VfpAnalysis {
    nx::map<u32, FunctionVfp> functions;  // by code address
    const FunctionVfp* at(u32 code) const {
        auto it = functions.find(code);
        return it == functions.end() ? nullptr : &it->second;
    }
};

VfpAnalysis vfp_analysis(const Program& program, const VfpOptions& options);

}  // namespace psvrecore::analysis
