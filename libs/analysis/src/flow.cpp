#include "psvrecore/analysis/flow.hpp"

namespace psvrecore::analysis {

using arm::Op;
using arm::OperandKind;

namespace {

bool writes_pc_as_destination(const arm::Instruction& in) {
    if (in.operand_count == 0) return false;
    const auto& d = in.operands[0];
    if (d.kind != OperandKind::Reg || d.reg != arm::kPc) return false;
    switch (in.op) {
        case Op::And: case Op::Eor: case Op::Sub: case Op::Rsb: case Op::Add: case Op::Adc: case Op::Sbc:
        case Op::Rsc: case Op::Orr: case Op::Mov: case Op::Bic: case Op::Mvn: case Op::Orn: case Op::Adr:
        case Op::Lsl: case Op::Lsr: case Op::Asr: case Op::Ror: case Op::Rrx:
        case Op::Ldr:
            return true;
        default: return false;  // (other destinations of PC are unpredictable: flagged by the decoder)
    }
}

}  // namespace

Flow flow_of(const arm::Instruction& in) {
    Flow f;
    f.conditional = is_conditional(in);
    const auto other_mode = [&](u32 address) {
        return in.mode == arm::Mode::Arm ? (address | 1u) : (address & ~3u);
    };
    switch (in.op) {
        case Op::Undefined:
        case Op::Udf:
        case Op::Bkpt:
        case Op::Subs:  // subs pc, lr: exception return
        case Op::Eret:
        case Op::Rfeda: case Op::Rfedb: case Op::Rfeia: case Op::Rfeib:
            f.kind = FlowKind::Trap;
            return f;
        case Op::B:
            f.kind = FlowKind::Branch;
            f.target = in.operands[0].imm | (in.mode == arm::Mode::Thumb ? 1u : 0u);
            return f;
        case Op::Cbz:
        case Op::Cbnz:
            f.kind = FlowKind::Branch;
            f.conditional = true;
            f.target = in.operands[1].imm | 1u;
            return f;
        case Op::Bl:
            f.kind = FlowKind::Call;
            f.target = in.operands[0].imm | (in.mode == arm::Mode::Thumb ? 1u : 0u);
            return f;
        case Op::Blx:
            if (in.operands[0].kind == OperandKind::Label) {
                f.kind = FlowKind::Call;
                f.target = other_mode(in.operands[0].imm);
            } else {
                f.kind = FlowKind::IndirectCall;
            }
            return f;
        case Op::Bx:
        case Op::Bxj:
        case Op::Tbb:
        case Op::Tbh:
            f.kind = FlowKind::IndirectBranch;
            return f;
        case Op::Pop:
            if (in.operands[0].imm & (1u << arm::kPc)) f.kind = FlowKind::IndirectBranch;
            return f;
        case Op::Ldm: case Op::Ldmib: case Op::Ldmda: case Op::Ldmdb:
            if (in.operands[1].imm & (1u << arm::kPc)) f.kind = (in.flags & arm::kUserRegs) ? FlowKind::Trap : FlowKind::IndirectBranch;
            return f;
        default:
            if (writes_pc_as_destination(in)) {
                // (an ARM data-processing instruction with S and PC as destination is an exception return)
                f.kind = (in.flags & arm::kSetsFlags) && in.op != Op::Ldr ? FlowKind::Trap : FlowKind::IndirectBranch;
            }
            return f;
    }
}

}  // namespace psvrecore::analysis
