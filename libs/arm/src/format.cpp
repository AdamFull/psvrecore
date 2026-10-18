#include "psvrecore/arm/format.hpp"

#include <bit>
#include <cstring>

#include "common.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::arm {

using namespace detail;

namespace {

constexpr const char* kMnemonics[] = {
#define PSVRECORE_ARM_OP_TEXT(name, text) text,
    PSVRECORE_ARM_OPS(PSVRECORE_ARM_OP_TEXT)
#undef PSVRECORE_ARM_OP_TEXT
};

constexpr const char* kRegisters[16] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7",
                                        "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
constexpr const char* kConds[16] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                                    "hi", "ls", "ge", "lt", "gt", "le", "", ""};
constexpr const char* kShifts[5] = {"lsl", "lsr", "asr", "ror", "rrx"};

nx::string hex(u32 v) { return nx::format("#0x{:x}", v); }
nx::string signed_hex(u32 v) {
    const i32 s = static_cast<i32>(v);
    return s < 0 ? nx::format("#-0x{:x}", 0u - v) : nx::format("#0x{:x}", v);
}

// How the reference disassembler prints an ARM modified immediate: the value, when the encoding is the one with the
// smallest rotation that represents it; otherwise the 8-bit value and the rotation.
int canonical_rotation(u32 value) {
    if ((value & ~0xFFu) == 0) return 0;
    const u32 tz = static_cast<u32>(std::countr_zero(value)) & ~1u;
    if ((ror32(value, tz) & ~0xFFu) == 0) return static_cast<int>((32 - tz) & 31);
    if (value & 63u) {
        const u32 tz2 = static_cast<u32>(std::countr_zero(value & ~63u)) & ~1u;
        if ((ror32(value, tz2) & ~0xFFu) == 0) return static_cast<int>((32 - tz2) & 31);
    }
    return static_cast<int>((32 - tz) & 31);
}
nx::string mod_imm_text(const Operand& o, bool as_unsigned) {
    const u32 rot = canonical_rotation(o.imm);
    const bool canonical = static_cast<u32>(o.amount) * 2 == rot || ((o.imm & ~0xFFu) == 0 && o.amount == 0);
    if (canonical) return as_unsigned ? nx::format("#{}", o.imm) : nx::format("#{}", static_cast<i32>(o.imm));
    return nx::format("#{}, #{}", o.index, o.amount * 2);
}

nx::string sysreg_text(u32 code) {
    switch (code) {
        case kSysApsr: return "apsr";
        case kSysCpsr: return "cpsr";
        case kSysSpsr: return "spsr";
        case kSysFpsid: return "fpsid";
        case kSysFpscr: return "fpscr";
        case kSysMvfr1: return "mvfr1";
        case kSysMvfr0: return "mvfr0";
        case kSysFpexc: return "fpexc";
        case kSysFpinst: return "fpinst";
        case kSysFpinst2: return "fpinst2";
        case kSysApsrNzcv: return "APSR_nzcv";
        default: break;
    }
    if (code >= kSysMsrBase) {
        const bool spsr = code & 0x10;
        const u32 mask = code & 0xF;
        if (!spsr && (mask == 8 || mask == 4 || mask == 12)) {
            if (mask == 8) return "apsr_nzcvq";
            if (mask == 4) return "apsr_g";
            return "apsr_nzcvqg";
        }
        if (mask == 0) return spsr ? "spsr" : "cpsr";
        nx::string s = spsr ? "spsr_" : "cpsr_";
        if (mask & 8) s += "f";
        if (mask & 4) s += "s";
        if (mask & 2) s += "x";
        if (mask & 1) s += "c";
        return s;
    }
    return "?";
}

nx::string barrier_text(Op op, u32 option) {
    if (op == Op::Isb) return option == 15 ? nx::string("sy") : hex(option);
    switch (option) {
        case 15: return "sy";
        case 14: return "st";
        case 11: return "ish";
        case 10: return "ishst";
        case 7: return "nsh";
        case 6: return "nshst";
        case 3: return "osh";
        case 2: return "oshst";
        default: return hex(option);
    }
}

nx::string shift_text(ShiftType s, u8 amount) {
    if (s == ShiftType::Rrx) return "rrx";
    return nx::format("{} #{}", kShifts[static_cast<u32>(s)], amount);
}

nx::string mem_text(const Operand& o) {
    nx::string s = nx::format("[{}", kRegisters[o.reg & 15]);
    if (o.count) s += nx::format(":{}", o.count * 8);  // Advanced SIMD alignment (count: bytes)
    const bool pre = o.flags & kMemPre, wb = o.flags & kMemWriteBack, sub = o.flags & kMemSubtract;
    nx::string offset;
    if (o.flags & kMemIndexReg) {
        offset = nx::format("{}{}", sub ? "-" : "", kRegisters[o.reg2 & 15]);
        if (o.shift != ShiftType::Lsl || o.amount) offset += ", " + shift_text(o.shift, o.amount);
    } else if (o.imm || sub || (o.flags & kMemImm)) {
        offset = nx::format("#{}0x{:x}", sub ? "-" : "", o.imm);
    }
    if (o.flags & kMemPostReg) return s + nx::format("], {}", kRegisters[o.reg2 & 15]);
    if (pre || !wb) {
        if (!offset.empty()) s += ", " + offset;
        s += "]";
        if (pre && wb) s += "!";
        return s;
    }
    s += "]";
    if (!offset.empty()) s += ", " + offset;
    return s;
}

// The element value of an Advanced SIMD modified immediate (AdvSIMDExpandImm, one element).
u64 simd_imm_value(const Operand& o) {
    const u64 imm8 = o.index;
    const u32 cmode = o.amount;
    switch (cmode >> 1) {
        case 0: return imm8;
        case 1: return imm8 << 8;
        case 2: return imm8 << 16;
        case 3: return imm8 << 24;
        case 4: return imm8;
        case 5: return imm8 << 8;
        case 6: return (cmode & 1) ? (imm8 << 16) | 0xFFFF : (imm8 << 8) | 0xFF;
        default:
            if (cmode == 0xE && o.count) {
                u64 v = 0;
                for (int i = 0; i < 8; ++i)
                    if (imm8 & (1u << i)) v |= u64{0xFF} << (8 * i);
                return v;
            }
            return imm8;
    }
}

nx::string float_text(u32 bits_value) {
    float f;
    std::memcpy(&f, &bits_value, 4);
    return nx::format("#{:e}", static_cast<double>(f));
}

nx::string list_text(const char* prefix, u8 first, u8 count, u8 spacing) {
    nx::string s = "{";
    for (u8 i = 0; i < count; ++i) {
        if (i) s += ", ";
        s += nx::format("{}{}", prefix, (first + i * spacing) & 31);
    }
    return s + "}";
}

nx::string operand_text(const Operand& o, const Instruction& in, usize index) {
    switch (o.kind) {
        case OperandKind::None: return {};
        case OperandKind::Reg: {
            nx::string s = kRegisters[o.reg & 15];
            if (index == 0 && (in.flags & kWriteBack)) s += "!";
            return s;
        }
        case OperandKind::RegShiftImm: return nx::format("{}, {}", kRegisters[o.reg & 15], shift_text(o.shift, o.amount));
        case OperandKind::RegShiftReg:
            return nx::format("{}, {} {}", kRegisters[o.reg & 15], kShifts[static_cast<u32>(o.shift)], kRegisters[o.reg2 & 15]);
        case OperandKind::Imm: return (o.flags & kSigned) ? signed_hex(o.imm) : hex(o.imm);
        case OperandKind::ModImm: {
            const bool as_unsigned = in.op == Op::Msr ||
                                     (in.op == Op::Mov && in.operand_count > 0 && in.operands[0].reg == kPc);
            return mod_imm_text(o, as_unsigned);
        }
        case OperandKind::Label: return nx::format("0x{:x}", o.imm);
        case OperandKind::Mem: return mem_text(o);
        case OperandKind::RegList: {
            nx::string s = "{";
            bool first = true;
            for (u32 r = 0; r < 16; ++r) {
                if (!(o.imm & (1u << r))) continue;
                if (!first) s += ", ";
                s += kRegisters[r];
                first = false;
            }
            s += "}";
            if (in.flags & kUserRegs) s += " ^";
            return s;
        }
        case OperandKind::SReg: return nx::format("s{}", o.reg);
        case OperandKind::DReg: {
            nx::string s = nx::format("d{}", o.reg);
            return s;
        }
        case OperandKind::QReg: return nx::format("q{}", o.reg);
        case OperandKind::SRegList: return list_text("s", o.reg, o.count, 1);
        case OperandKind::DRegList: return list_text("d", o.reg, o.count, o.index ? o.index : 1);
        case OperandKind::DScalar: return nx::format("d{}[{}]", o.reg, o.index);
        case OperandKind::DLaneList: {
            nx::string s = "{";
            for (u8 i = 0; i < o.count; ++i) {
                if (i) s += ", ";
                const u32 r = (o.reg + i * (o.amount ? o.amount : 1)) & 31;
                s += (o.flags & kAllLanes) ? nx::format("d{}[]", r) : nx::format("d{}[{}]", r, o.index);
            }
            return s + "}";
        }
        case OperandKind::CoprocNum: return nx::format("p{}", o.imm);
        case OperandKind::CoprocReg: return nx::format("c{}", o.reg);
        case OperandKind::CoprocOption: return nx::format("{{{}}}", o.imm);
        case OperandKind::Sysreg: return sysreg_text(o.imm);
        case OperandKind::Barrier: return barrier_text(in.op, o.imm);
        case OperandKind::Text:
            if (o.imm == kTextBe) return "be";
            if (o.imm == kTextLe) return "le";
            if (o.imm >= kTextIflagsBase && o.imm < kTextIflagsBase + 8) {
                const u32 f = o.imm - kTextIflagsBase;
                nx::string s;
                if (f & 4) s += "a";
                if (f & 2) s += "i";
                if (f & 1) s += "f";
                return s.empty() ? nx::string("none") : s;
            }
            return "?";
        case OperandKind::FloatImm: return float_text(o.imm);
        case OperandKind::SimdImm: {
            if (o.amount == 0xF && !o.count) {  // vmov.f32: abcdefgh as a VFP immediate
                const u32 imm8 = o.index;
                const u32 b6 = (imm8 >> 6) & 1;
                const u32 exp = ((b6 ^ 1) << 7) | (b6 ? 0x7Cu : 0u) | ((imm8 >> 4) & 3);
                return float_text((((imm8 >> 7) & 1) << 31) | (exp << 23) | ((imm8 & 0xF) << 19));
            }
            return nx::format("#0x{:x}", simd_imm_value(o));
        }
    }
    return "?";
}

// it{t|e}{t|e}{t|e} <firstcond>: the mask's bits above its lowest set bit, compared with firstcond's low bit.
nx::string it_text(const Instruction& in) {
    const u32 mask = in.operands[0].imm, firstcond = static_cast<u32>(in.cond);
    nx::string s = "it";
    const int lowest = std::countr_zero(mask);
    for (int b = 3; b > lowest; --b) s += ((mask >> b) & 1) == (firstcond & 1) ? "t" : "e";
    return s + "\t" + kConds[firstcond] + (firstcond == 14 ? "al" : "");
}

}  // namespace

const char* mnemonic(Op op) { return kMnemonics[static_cast<u32>(op)]; }
const char* register_name(u8 r) { return kRegisters[r & 15]; }
const char* cond_name(Cond c) { return kConds[static_cast<u32>(c)]; }

const char* dt_name(Dt dt) {
    switch (dt) {
        case Dt::None: return "";
        case Dt::S8: return "s8";
        case Dt::S16: return "s16";
        case Dt::S32: return "s32";
        case Dt::S64: return "s64";
        case Dt::U8: return "u8";
        case Dt::U16: return "u16";
        case Dt::U32: return "u32";
        case Dt::U64: return "u64";
        case Dt::I8: return "i8";
        case Dt::I16: return "i16";
        case Dt::I32: return "i32";
        case Dt::I64: return "i64";
        case Dt::F16: return "f16";
        case Dt::F32: return "f32";
        case Dt::F64: return "f64";
        case Dt::P8: return "p8";
        case Dt::P16: return "p16";
        case Dt::B8: return "8";
        case Dt::B16: return "16";
        case Dt::B32: return "32";
        case Dt::B64: return "64";
    }
    return "";
}

nx::string format(const Instruction& in) {
    if (in.op == Op::Undefined) return "<unknown>";
    if (in.op == Op::It) return it_text(in);
    // Two permanently undefined encodings the reference names (trap; Windows' divide-by-zero break).
    if (in.op == Op::Udf && in.operand_count == 1) {
        const u32 v = in.operands[0].imm;
        if ((in.mode == Mode::Thumb && in.size == 2 && v == 0xFE) || (in.mode == Mode::Arm && v == 0xFDEE)) return "trap";
        if (in.mode == Mode::Thumb && in.size == 2 && v == 0xF9) return "__brkdiv0";
    }
    nx::string s = mnemonic(in.op);
    if (in.flags & kXForm) {  // fldmiax / fldmdbx / fstmiax / fstmdbx
        s = in.op == Op::Vldmia ? "fldmia" : in.op == Op::Vldmdb ? "fldmdb" : in.op == Op::Vstmia ? "fstmia" : "fstmdb";
        s += "x";
    }
    if (in.flags & kImm12) s += "w";
    if (in.flags & kSetsFlags) s += "s";
    s += kConds[static_cast<u32>(in.cond)];
    if (in.dt != Dt::None) s += nx::format(".{}", dt_name(in.dt));
    if (in.dt2 != Dt::None) s += nx::format(".{}", dt_name(in.dt2));
    if (in.flags & kWide) s += ".w";
    if (in.flags & kNarrow) s += ".n";
    for (usize i = 0; i < in.operand_count; ++i) {
        s += i == 0 ? "\t" : ", ";
        s += operand_text(in.operands[i], in, i);
    }
    return s;
}

}  // namespace psvrecore::arm
