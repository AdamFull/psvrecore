#pragma once

// The code generator's per-function state, shared by the files that translate each group of instructions.

#include "psvrecore/analysis/cfg.hpp"
#include "psvrecore/analysis/flags.hpp"
#include "psvrecore/analysis/flow.hpp"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/arm/instruction.hpp"
#include "psvrecore/recomp/generator.hpp"
#include "core/foundation/containers/hash_set.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::recomp::detail {

using arm::Instruction;
using arm::Op;
using arm::Operand;
using arm::OperandKind;

inline nx::string hex(u32 v) { return nx::format("0x{:08X}u", v); }
inline nx::string dec(u32 v) { return nx::format("{}", v); }
inline nx::string label_name(u32 address) { return nx::format("L_{:08X}", address & ~1u); }
inline nx::string R(u32 r) { return nx::format("ctx->r[{}]", r); }

using analysis::FlagSet;

// The C condition of an ARM condition code ("" for always), on the function's flags (the local `fl`).
nx::string condition(arm::Cond c);

class Emitter {
public:
    Emitter(const analysis::Program& program, const analysis::Function& fn, const analysis::FlagLiveness& flags,
            const nx::string& prefix, Stats& stats);

    // The function's C definition.
    nx::string function();

    // --- used by the instruction translators ---

    // Appends a statement line (inside the current instruction's block).
    void line(nx::string_view text) {
        body_ += "        ";
        body_ += text;
        body_ += '\n';
    }
    // A fresh temporary name.
    nx::string temp(const char* stem = "t") { return nx::format("{}{}", stem, temp_counter_++); }
    // Ends the instruction with a trap (and counts it under `what`).
    void trap(const Instruction& in, const char* kind, nx::string_view what);
    void unimplemented(const Instruction& in);

    // The value of a core register as an operand of `in` (the PC reads as the instruction's address plus 8 or 4).
    nx::string read(const Instruction& in, u32 r) const;
    // Writes a core register; a write to the PC is a branch. `kind`: how the PC write treats bit 0 (see branch_to_value).
    enum class PcWrite { Interworking, Branch };
    void write(const Instruction& in, u32 r, nx::string_view value, PcWrite kind = PcWrite::Interworking);
    // A branch to a computed code address (the instruction's jump table if it has one). `target` is an expression,
    // evaluated once; a table branch evaluates it only for an index outside its table (it may read guest memory).
    void branch_to_value(const Instruction& in, nx::string_view target);
    // A table branch: a switch on the index register (see analysis::JumpTable).
    void table_branch(const Instruction& in, const analysis::JumpTable& table, nx::string_view target);
    // A branch to a known code address: a goto, or a tail call.
    void branch_to(const Instruction& in, u32 target_code);
    // A call: LR = the return address, then the function, the import or the runtime's lookup.
    void call(const Instruction& in, u32 target_code);

    // Flexible second operand: its value, and its carry out when `carry` is given (else unused).
    nx::string operand2(const Instruction& in, const Operand& o, nx::string* carry);
    // A memory operand's address. `writeback` receives the statement updating the base register (empty if none).
    nx::string address(const Instruction& in, const Operand& m, nx::string* writeback);

    // Where the module's code builds an imported variable's address (movw/movt patched by the loader): the C
    // expression of that address, and the patch type; empty elsewhere.
    nx::string variable_address(u32 site, u8& type) const;

    // Literal pools: a load from a read-only segment of the module can be done at generation time.
    bool constant_load(u32 address, u32 size, u32& value) const;

    // --- condition flags (analysis/flags.hpp): kept in the local `fl`, computed only where they are live ---

    // The flags live after the current instruction's own effect: of those it writes, the ones to compute.
    FlagSet live() const { return flags_fn_->after[index_]; }
    // fl.n and fl.z from a 32-bit result, those of `which` that are live.
    void set_nz(nx::string_view r, FlagSet which);
    // const uint32_t r = x + y + carry_in (AddWithCarry), setting the flags of `which` that are live.
    void add_with_carry(nx::string_view r, nx::string_view x, nx::string_view y, nx::string_view carry_in, FlagSet which);
    // Writes flags to the context (before something outside the function may read them), and reads them back.
    void store_flags(FlagSet f);
    void load_flags(FlagSet f);
    // Around the current instruction's call (to a function, an import, through the runtime, svc): the flags the
    // callee may read, then those read back after it (or a trap after a function that never returns).
    void call_flags_before();
    void call_flags_after(const Instruction& in);
    // Before a trap: every local flag in the context (what the function computed; see analysis::FunctionFlags).
    void store_flags_for_trap() { store_flags(analysis::kAllFlags); }

    u32 next_code(const Instruction& in) const {
        return (in.address + in.size) | (in.mode == arm::Mode::Thumb ? 1u : 0u);
    }
    const analysis::Program& program() const { return program_; }
    const analysis::Function& fn() const { return fn_; }
    Stats& stats() { return stats_; }

private:
    void instruction(const Instruction& in);
    bool falls_through(const Instruction& in) const;
    void count_flags(const Instruction& in);
    nx::string flags_line(FlagSet f, bool store) const;

    const analysis::Program& program_;
    const analysis::Function& fn_;
    const analysis::FlagLiveness& flags_;
    const analysis::FunctionFlags* flags_fn_;
    analysis::Cfg cfg_;
    usize index_ = 0;  // the current instruction's, in fn_.code
    const nx::string& prefix_;
    Stats& stats_;
    nx::string body_;
    u32 temp_counter_ = 0;
    nx::flat_hash_set<u32> labels_;
    bool ended_ = false;  // the current instruction's translation ends the flow (trap)
};

// The translators, by instruction group; each returns false when the instruction is not in its group.
bool emit_core(Emitter& e, const Instruction& in);
bool emit_memory(Emitter& e, const Instruction& in);
bool emit_vfp(Emitter& e, const Instruction& in);
bool emit_simd(Emitter& e, const Instruction& in);

}  // namespace psvrecore::recomp::detail
