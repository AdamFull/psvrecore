#pragma once

// Bit-field helpers and operand builders shared by the decoders.

#include <initializer_list>

#include "psvrecore/arm/decode.hpp"

namespace psvrecore::arm::detail {

constexpr u32 bits(u32 v, int hi, int lo) { return (v >> lo) & ((hi - lo == 31) ? ~0u : ((1u << (hi - lo + 1)) - 1)); }
constexpr bool bit(u32 v, int n) { return (v >> n) & 1; }
constexpr u32 ror32(u32 v, u32 r) { return r == 0 ? v : (v >> (r & 31)) | (v << ((32 - r) & 31)); }
constexpr u32 sign_extend(u32 v, int width) {
    const u32 m = 1u << (width - 1);
    return (v ^ m) - m;
}

inline void set(Instruction& in, Op op, std::initializer_list<Operand> operands) {
    in.op = op;
    in.operand_count = 0;
    for (const auto& o : operands) in.add(o);
}

inline void undefined(Instruction& in) {
    in.op = Op::Undefined;
    in.operand_count = 0;
    in.flags = 0;
    in.dt = in.dt2 = Dt::None;
}

inline void unpredictable(Instruction& in, bool when = true) {
    if (when) in.flags |= kUnpredictable;
}

// DecodeImmShift: a register shifted by an immediate; LSL #0 is the plain register.
inline Operand shifted_reg(u8 rm, u32 type, u32 imm5) {
    Operand o{OperandKind::RegShiftImm, rm};
    switch (type) {
        case 0:
            if (imm5 == 0) return reg(rm);
            o.shift = ShiftType::Lsl;
            o.amount = static_cast<u8>(imm5);
            break;
        case 1:
            o.shift = ShiftType::Lsr;
            o.amount = static_cast<u8>(imm5 ? imm5 : 32);
            break;
        case 2:
            o.shift = ShiftType::Asr;
            o.amount = static_cast<u8>(imm5 ? imm5 : 32);
            break;
        default:
            if (imm5 == 0) {
                o.shift = ShiftType::Rrx;
            } else {
                o.shift = ShiftType::Ror;
                o.amount = static_cast<u8>(imm5);
            }
            break;
    }
    return o;
}

inline Operand reg_shifted_reg(u8 rm, u32 type, u8 rs) {
    Operand o{OperandKind::RegShiftReg, rm};
    o.shift = static_cast<ShiftType>(type);
    o.reg2 = rs;
    return o;
}

// [rn, #+/-offset], [rn, #+/-offset]!, [rn], #+/-offset
inline Operand mem_imm(u8 rn, u32 offset, bool add, bool index, bool wback) {
    Operand o{OperandKind::Mem, rn};
    o.imm = offset;
    o.flags = static_cast<u8>((index ? kMemPre : 0) | ((wback || !index) ? kMemWriteBack : 0) | (add ? 0 : kMemSubtract));
    return o;
}

// [rn, +/-rm{, shift}], with !, or post-indexed
inline Operand mem_reg(u8 rn, u8 rm, bool add, bool index, bool wback, ShiftType shift = ShiftType::Lsl, u8 amount = 0) {
    Operand o{OperandKind::Mem, rn};
    o.reg2 = rm;
    o.shift = shift;
    o.amount = amount;
    o.flags = static_cast<u8>(kMemIndexReg | (index ? kMemPre : 0) | ((wback || !index) ? kMemWriteBack : 0) |
                              (add ? 0 : kMemSubtract));
    return o;
}

inline Operand reg_list(u32 mask) {
    Operand o{OperandKind::RegList};
    o.imm = mask;
    return o;
}

inline Operand coproc(u32 n) {
    Operand o{OperandKind::CoprocNum};
    o.imm = n;
    return o;
}
inline Operand creg(u32 n) { return Operand{OperandKind::CoprocReg, static_cast<u8>(n)}; }

inline Operand text(u32 code) {
    Operand o{OperandKind::Text};
    o.imm = code;
    return o;
}
inline Operand sysreg(u32 code) {
    Operand o{OperandKind::Sysreg};
    o.imm = code;
    return o;
}

// The Text and Sysreg codes the formatter knows.
enum TextCode : u32 {
    kTextBe = 1,
    kTextLe,
    kTextIflagsBase = 0x10,    // + aif bits (cps)
    kTextItBase = 0x100,       // + mask (it: the then/else pattern; with Instruction::cond the first condition)
};
enum SysregCode : u32 {
    kSysApsr = 1, kSysCpsr, kSysSpsr, kSysFpsid, kSysFpscr, kSysMvfr1, kSysMvfr0, kSysFpexc, kSysFpinst, kSysFpinst2,
    kSysApsrNzcv,               // vmrs APSR_nzcv, fpscr
    kSysFpReg,                  // vmrs/vmsr with another register number: index = reg number
    kSysMsrBase = 0x100,        // + (R << 4) | mask: msr fields
};

// Shared between the ARM and Thumb decoders.
Op parallel_op(bool is_unsigned, u32 prefix, u32 op);  // prefix 1 normal, 2 saturating, 3 halving; op 0..7
void decode_coprocessor(u32 word, Instruction& in, bool unconditional);

}  // namespace psvrecore::arm::detail
