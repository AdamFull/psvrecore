#pragma once

// The code generator's per-function state, shared by the files that translate each group of instructions.

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

// The C condition of an ARM condition code ("" for always).
nx::string condition(arm::Cond c);

class Emitter {
public:
    Emitter(const analysis::Program& program, const analysis::Function& fn, const nx::string& prefix, Stats& stats)
        : program_(program), fn_(fn), prefix_(prefix), stats_(stats) {}

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
    // A branch to a computed code address (the instruction's jump table if it has one).
    void branch_to_value(const Instruction& in, nx::string_view target);
    // A branch to a known code address: a goto, or a tail call.
    void branch_to(const Instruction& in, u32 target_code);
    // A call: LR = the return address, then the function, the import or the runtime's lookup.
    void call(const Instruction& in, u32 target_code);

    // Flexible second operand: its value, and its carry out when `carry` is given (else unused).
    nx::string operand2(const Instruction& in, const Operand& o, nx::string* carry);
    // A memory operand's address. `writeback` receives the statement updating the base register (empty if none).
    nx::string address(const Instruction& in, const Operand& m, nx::string* writeback);

    // Literal pools: a load from a read-only segment of the module can be done at generation time.
    bool constant_load(u32 address, u32 size, u32& value) const;

    u32 next_code(const Instruction& in) const {
        return (in.address + in.size) | (in.mode == arm::Mode::Thumb ? 1u : 0u);
    }
    const analysis::Program& program() const { return program_; }
    const analysis::Function& fn() const { return fn_; }
    Stats& stats() { return stats_; }

private:
    void instruction(const Instruction& in);
    bool falls_through(const Instruction& in) const;

    const analysis::Program& program_;
    const analysis::Function& fn_;
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
