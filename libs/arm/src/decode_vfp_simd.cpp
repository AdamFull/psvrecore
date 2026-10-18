// VFPv3 and Advanced SIMD (ARMv7-A, A7), shared by the ARM and Thumb decoders. Words are in the ARM form; the
// Thumb decoder converts its encodings first.

#include "common.hpp"

namespace psvrecore::arm {

using namespace detail;

namespace {

Operand sreg(u32 n) { return Operand{OperandKind::SReg, static_cast<u8>(n)}; }
Operand dreg(u32 n) { return Operand{OperandKind::DReg, static_cast<u8>(n)}; }
Operand qreg(u32 n) { return Operand{OperandKind::QReg, static_cast<u8>(n)}; }
Operand dscalar(u32 n, u32 index) {
    Operand o{OperandKind::DScalar, static_cast<u8>(n)};
    o.index = static_cast<u8>(index);
    return o;
}
Operand dlist(u32 first, u32 count, u32 spacing = 1) {
    Operand o{OperandKind::DRegList, static_cast<u8>(first)};
    o.count = static_cast<u8>(count);
    o.index = static_cast<u8>(spacing);
    return o;
}
Operand slist(u32 first, u32 count) {
    Operand o{OperandKind::SRegList, static_cast<u8>(first)};
    o.count = static_cast<u8>(count);
    return o;
}

// Register numbers from a 4-bit field and its extra bit: Sn = field:bit; Dn = bit:field.
u32 s_num(u32 w, int hi, int lo, int extra) { return (bits(w, hi, lo) << 1) | (bit(w, extra) ? 1u : 0u); }
u32 d_num(u32 w, int hi, int lo, int extra) { return (bit(w, extra) ? 16u : 0u) | bits(w, hi, lo); }

constexpr Dt kS[4] = {Dt::S8, Dt::S16, Dt::S32, Dt::S64};
constexpr Dt kU[4] = {Dt::U8, Dt::U16, Dt::U32, Dt::U64};
constexpr Dt kI[4] = {Dt::I8, Dt::I16, Dt::I32, Dt::I64};
constexpr Dt kB[4] = {Dt::B8, Dt::B16, Dt::B32, Dt::B64};

// VFPExpandImm for single precision (double-precision immediates are the same values).
u32 vfp_expand_imm(u32 imm8) {
    const u32 sign = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
    const u32 exp = ((b6 ^ 1) << 7) | (b6 ? 0x7Cu : 0u) | ((imm8 >> 4) & 3);
    return (sign << 31) | (exp << 23) | ((imm8 & 0xF) << 19);
}

// --- VFP data processing (A7.5) ---

bool vfp_data(u32 w, Instruction& in) {
    const u32 opc1 = (bit(w, 23) << 2) | bits(w, 21, 20), opc2 = bits(w, 19, 16), opc3 = bits(w, 7, 6);
    const bool dbl = bit(w, 8);
    const u32 d = dbl ? d_num(w, 15, 12, 22) : s_num(w, 15, 12, 22);
    const u32 n = dbl ? d_num(w, 19, 16, 7) : s_num(w, 19, 16, 7);
    const u32 m = dbl ? d_num(w, 3, 0, 5) : s_num(w, 3, 0, 5);
    const auto r = [&](u32 x) { return dbl ? dreg(x) : sreg(x); };
    const Dt fdt = dbl ? Dt::F64 : Dt::F32;
    in.dt = fdt;
    const bool op6 = bit(w, 6);
    switch (opc1) {
        case 0: set(in, op6 ? Op::Vmls : Op::Vmla, {r(d), r(n), r(m)}); return true;
        case 1: set(in, op6 ? Op::Vnmla : Op::Vnmls, {r(d), r(n), r(m)}); return true;
        case 2: set(in, op6 ? Op::Vnmul : Op::Vmul, {r(d), r(n), r(m)}); return true;
        case 3: set(in, op6 ? Op::Vsub : Op::Vadd, {r(d), r(n), r(m)}); return true;
        case 4:
            if (op6) return false;
            set(in, Op::Vdiv, {r(d), r(n), r(m)});
            return true;
        case 7: break;
        default: return false;  // (5, 6: fused multiply-accumulate, VFPv4: not on the Cortex-A9)
    }
    if ((opc3 & 1) == 0) {  // vmov (immediate)
        if (bits(w, 7, 4) != 0) return false;
        Operand f{OperandKind::FloatImm};
        f.imm = vfp_expand_imm((opc2 << 4) | bits(w, 3, 0));
        set(in, Op::Vmov, {r(d), f});
        return true;
    }
    switch (opc2) {
        case 0: set(in, opc3 == 1 ? Op::Vmov : Op::Vabs, {r(d), r(m)}); return true;
        case 1: set(in, opc3 == 1 ? Op::Vneg : Op::Vsqrt, {r(d), r(m)}); return true;
        case 2:
        case 3: {  // vcvtb / vcvtt between half and single precision
            if (dbl) return false;
            const bool to_half = bit(w, 16);
            set(in, bit(w, 7) ? Op::Vcvtt : Op::Vcvtb, {sreg(d), sreg(m)});
            in.dt = to_half ? Dt::F16 : Dt::F32;
            in.dt2 = to_half ? Dt::F32 : Dt::F16;
            return true;
        }
        case 4:
        case 5: {
            const Op op = bit(w, 7) ? Op::Vcmpe : Op::Vcmp;
            if (opc2 == 5) {
                if (bits(w, 5, 0) != 0) return false;
                set(in, op, {r(d), imm(0)});
                return true;
            }
            set(in, op, {r(d), r(m)});
            return true;
        }
        case 7:
            if (opc3 != 3) return false;
            if (dbl) {  // double to single
                set(in, Op::Vcvt, {sreg(s_num(w, 15, 12, 22)), dreg(m)});
                in.dt = Dt::F32;
                in.dt2 = Dt::F64;
            } else {
                set(in, Op::Vcvt, {dreg(d_num(w, 15, 12, 22)), sreg(m)});
                in.dt = Dt::F64;
                in.dt2 = Dt::F32;
            }
            return true;
        case 8: {  // integer to floating point
            const u32 sm = s_num(w, 3, 0, 5);
            set(in, Op::Vcvt, {r(d), sreg(sm)});
            in.dt = fdt;
            in.dt2 = bit(w, 7) ? Dt::S32 : Dt::U32;
            return true;
        }
        case 10:
        case 11:
        case 14:
        case 15: {  // fixed point <-> floating point: vcvt.f32.s16 s0, s0, #imm
            const bool to_fixed = bit(w, 18), u = bit(w, 16), sx = bit(w, 7);
            const u32 size = sx ? 32 : 16;
            const u32 imm4i = (bits(w, 3, 0) << 1) | (bit(w, 5) ? 1u : 0u);
            set(in, Op::Vcvt, {r(d), r(d), imm(size - imm4i, kSigned)});
            const Dt fixed = u ? (sx ? Dt::U32 : Dt::U16) : (sx ? Dt::S32 : Dt::S16);
            in.dt = to_fixed ? fixed : fdt;
            in.dt2 = to_fixed ? fdt : fixed;
            unpredictable(in, imm4i > size);
            return true;
        }
        case 12:
        case 13: {  // floating point to integer
            set(in, bit(w, 7) ? Op::Vcvt : Op::Vcvtr, {sreg(s_num(w, 15, 12, 22)), r(m)});
            in.dt = bit(w, 16) ? Dt::S32 : Dt::U32;
            in.dt2 = fdt;
            return true;
        }
        default: return false;
    }
}

// --- 8-, 16-, 32- and 64-bit transfers, and extension register loads/stores (A7.6, A7.8, A7.9) ---

bool vfp_load_store(u32 w, Instruction& in) {
    const u32 opcode = bits(w, 24, 20);
    const bool dbl = bit(w, 8);
    const u8 rn = static_cast<u8>(bits(w, 19, 16));
    const u32 imm8 = bits(w, 7, 0);
    if ((opcode & 0x1E) == 0x04) {  // 64-bit transfers
        const bool to_core = bit(w, 20);
        const u8 rt = static_cast<u8>(bits(w, 15, 12)), rt2 = rn;
        if (bits(w, 7, 6) != 0 || !bit(w, 4)) return false;
        if (!dbl) {
            const u32 m = s_num(w, 3, 0, 5);
            if (m == 31) return false;
            if (to_core) set(in, Op::Vmov, {reg(rt), reg(rt2), sreg(m), sreg(m + 1)});
            else set(in, Op::Vmov, {sreg(m), sreg(m + 1), reg(rt), reg(rt2)});
        } else {
            const u32 m = d_num(w, 3, 0, 5);
            if (to_core) set(in, Op::Vmov, {reg(rt), reg(rt2), dreg(m)});
            else set(in, Op::Vmov, {dreg(m), reg(rt), reg(rt2)});
        }
        unpredictable(in, rt == kPc || rt2 == kPc || (to_core && rt == rt2));
        return true;
    }
    const bool p = bit(w, 24), u = bit(w, 23), wb = bit(w, 21), load = bit(w, 20);
    const u32 d = dbl ? d_num(w, 15, 12, 22) : s_num(w, 15, 12, 22);
    if ((opcode & 0x12) == 0x10) {  // vldr / vstr
        Operand m = mem_imm(rn, imm8 * 4, u, true, false);
        set(in, load ? Op::Vldr : Op::Vstr, {dbl ? dreg(d) : sreg(d), m});
        return true;
    }
    // vldm / vstm (and their fldmx/fstmx forms with an odd word count)
    if (p == u) return false;
    if (p && !wb) return false;
    if (wb) in.flags |= kWriteBack;
    const bool x_form = dbl && (imm8 & 1);  // fldmx/fstmx: an odd word count
    const bool push_pop = rn == kSp && wb && ((!load && p) || (load && !p)) && !x_form;
    Op op;
    if (push_pop) op = load ? Op::Vpop : Op::Vpush;
    else op = load ? (p ? Op::Vldmdb : Op::Vldmia) : (p ? Op::Vstmdb : Op::Vstmia);
    // Register counts out of range are unpredictable; the reference clamps them to a valid list. Its fldmx/fstmx
    // forms take only d0-d15 and clamp at d31 only.
    if (x_form && d >= 16) return false;
    u32 count = dbl ? imm8 / 2 : imm8;
    if (count == 0 || (dbl && count > 16) || d + count > 32) {
        count = d + count > 32 ? 32 - d : count;
        count = count == 0 ? 1 : count;
        unpredictable(in);
    }
    const Operand list = dbl ? dlist(d, count) : slist(d, count);
    if (push_pop) {
        in.flags &= ~kWriteBack;
        set(in, op, {list});
    } else {
        set(in, op, {reg(rn), list});
    }
    if (x_form) in.flags |= kXForm;
    unpredictable(in, rn == kPc && wb);
    return true;
}

bool vfp_core_transfer(u32 w, Instruction& in) {
    const bool l = bit(w, 20), c = bit(w, 8);
    const u32 a = bits(w, 23, 21), b = bits(w, 6, 5);
    const u8 rt = static_cast<u8>(bits(w, 15, 12));
    if (!c && a == 0) {  // vmov between a core register and a single-precision register
        if (bits(w, 6, 5) != 0 || bits(w, 3, 0) != 0) return false;
        const u32 n = s_num(w, 19, 16, 7);
        if (l) set(in, Op::Vmov, {reg(rt), sreg(n)});
        else set(in, Op::Vmov, {sreg(n), reg(rt)});
        return true;
    }
    if (!c && a == 7) {  // vmrs / vmsr
        static constexpr u32 kNames[16] = {kSysFpsid, kSysFpscr, 0, 0, 0, 0, kSysMvfr1, kSysMvfr0,
                                           kSysFpexc, kSysFpinst, kSysFpinst2, 0, 0, 0, 0, 0};
        const u32 spec = bits(w, 19, 16);
        if (kNames[spec] == 0) return false;
        if (l) set(in, Op::Vmrs, {rt == kPc && spec == 1 ? sysreg(kSysApsrNzcv) : reg(rt), sysreg(kNames[spec])});
        else if (kNames[spec] == kSysMvfr0 || kNames[spec] == kSysMvfr1) return false;
        else set(in, Op::Vmsr, {sysreg(kNames[spec]), reg(rt)});
        return true;
    }
    if (!c) return false;
    const u32 d = d_num(w, 19, 16, 7);
    if (!l && (a & 4)) {  // vdup (core register)
        if (bits(w, 3, 0) != 0 || bit(w, 6)) return false;
        const bool q = bit(w, 21);
        const u32 be = (bit(w, 22) << 1) | (bit(w, 5) ? 1u : 0u);
        static constexpr Dt kDt[4] = {Dt::B32, Dt::B16, Dt::B8, Dt::None};
        if (kDt[be] == Dt::None || (q && (d & 1))) return false;
        set(in, Op::Vdup, {q ? qreg(d >> 1) : dreg(d), reg(rt)});
        in.dt = kDt[be];
        return true;
    }
    // vmov between a core register and a scalar
    const u32 opc = (bits(w, 22, 21) << 2) | b;
    Dt dt;
    u32 index;
    if (opc & 8) {
        dt = Dt::B8;
        index = opc & 7;
    } else if (opc & 1) {
        dt = Dt::B16;
        index = (opc >> 1) & 3;
    } else if ((opc & 3) == 0) {
        dt = Dt::B32;
        index = (opc >> 2) & 1;
    } else {
        return false;
    }
    if (bits(w, 3, 0) != 0) return false;
    if (l) {
        const bool unsigned_ = bit(w, 23);
        if (dt == Dt::B32 && unsigned_) return false;
        set(in, Op::Vmov, {reg(rt), dscalar(d, index)});
        in.dt = dt == Dt::B8 ? (unsigned_ ? Dt::U8 : Dt::S8) : dt == Dt::B16 ? (unsigned_ ? Dt::U16 : Dt::S16) : Dt::B32;
    } else {
        if (bit(w, 23)) return false;
        set(in, Op::Vmov, {dscalar(d, index), reg(rt)});
        in.dt = dt;
    }
    return true;
}

// --- Advanced SIMD data processing (A7.4) ---

struct Regs {
    bool q;
    u32 d, n, m;
};
Regs regs3(u32 w) { return {bit(w, 6), d_num(w, 15, 12, 22), d_num(w, 19, 16, 7), d_num(w, 3, 0, 5)}; }
Operand vreg(bool q, u32 n) { return q ? qreg(n >> 1) : dreg(n); }

// Three registers of the same length (A7.4.1).
bool simd_three_same(u32 w, Instruction& in) {
    const u32 a = bits(w, 11, 8), size = bits(w, 21, 20);
    const bool b = bit(w, 4), u = bit(w, 24);
    const Regs r = regs3(w);
    if (r.q && ((r.d | r.n | r.m) & 1)) return false;
    const auto three = [&](Op op, Dt dt) {
        set(in, op, {vreg(r.q, r.d), vreg(r.q, r.n), vreg(r.q, r.m)});
        in.dt = dt;
        return true;
    };
    const auto shift_three = [&](Op op, Dt dt) {  // vshl and friends: dd, dm, dn
        set(in, op, {vreg(r.q, r.d), vreg(r.q, r.m), vreg(r.q, r.n)});
        in.dt = dt;
        return true;
    };
    const Dt sdt = u ? kU[size] : kS[size];
    const bool sz32 = !bit(w, 20);  // float operations: size<0> must be 0
    switch (a) {
        case 0:
            if (size == 3 && !b) return false;
            return three(b ? Op::Vqadd : Op::Vhadd, sdt);
        case 1:
            if (!b) {
                if (size == 3) return false;
                return three(Op::Vrhadd, sdt);
            }
            if (!u) {
                static constexpr Op k[4] = {Op::Vand, Op::Vbic, Op::Vorr, Op::Vorn};
                return three(k[size], Dt::None);
            } else {
                static constexpr Op k[4] = {Op::Veor, Op::Vbsl, Op::Vbit, Op::Vbif};
                return three(k[size], Dt::None);
            }
        case 2:
            if (size == 3 && !b) return false;
            return three(b ? Op::Vqsub : Op::Vhsub, sdt);
        case 3:
            if (size == 3) return false;
            return three(b ? Op::Vcge : Op::Vcgt, sdt);
        case 4: return shift_three(b ? Op::Vqshl : Op::Vshl, sdt);
        case 5: return shift_three(b ? Op::Vqrshl : Op::Vrshl, sdt);
        case 6:
            if (size == 3) return false;
            return three(b ? Op::Vmin : Op::Vmax, sdt);
        case 7:
            if (size == 3) return false;
            return three(b ? Op::Vaba : Op::Vabd, sdt);
        case 8:
            if (!b) return three(u ? Op::Vsub : Op::Vadd, kI[size]);
            if (size == 3) return false;
            return three(u ? Op::Vceq : Op::Vtst, u ? kI[size] : kB[size]);
        case 9:
            if (size == 3) return false;
            if (!b) return three(u ? Op::Vmls : Op::Vmla, kI[size]);
            if (u && size != 0) return false;
            return three(Op::Vmul, u ? Dt::P8 : kI[size]);
        case 10:
            if (size == 3 || r.q) return false;
            return three(b ? Op::Vpmin : Op::Vpmax, sdt);
        case 11:
            if (!b) {
                if (size == 0 || size == 3) return false;
                return three(u ? Op::Vqrdmulh : Op::Vqdmulh, kS[size]);
            }
            if (u || size == 3 || r.q) return false;
            return three(Op::Vpadd, kI[size]);
        case 12: return false;  // (fused multiply-accumulate: VFPv4)
        case 13:
            if (!sz32) return false;
            if (!b) {
                if (!u) return three(bit(w, 21) ? Op::Vsub : Op::Vadd, Dt::F32);
                if (!bit(w, 21) && r.q) return false;
                return three(bit(w, 21) ? Op::Vabd : Op::Vpadd, Dt::F32);
            }
            if (!u) return three(bit(w, 21) ? Op::Vmls : Op::Vmla, Dt::F32);
            if (bit(w, 21)) return false;
            return three(Op::Vmul, Dt::F32);
        case 14:
            if (!sz32) return false;
            if (!b) {
                if (!u) {
                    if (bit(w, 21)) return false;
                    return three(Op::Vceq, Dt::F32);
                }
                return three(bit(w, 21) ? Op::Vcgt : Op::Vcge, Dt::F32);
            }
            if (!u) return false;
            return three(bit(w, 21) ? Op::Vacgt : Op::Vacge, Dt::F32);
        default:
            if (!sz32) return false;
            if (!b) {
                if (!u) return three(bit(w, 21) ? Op::Vmin : Op::Vmax, Dt::F32);
                if (r.q) return false;
                return three(bit(w, 21) ? Op::Vpmin : Op::Vpmax, Dt::F32);
            }
            if (u) return false;
            return three(bit(w, 21) ? Op::Vrsqrts : Op::Vrecps, Dt::F32);
    }
}

// Three registers of different lengths (A7.4.2).
bool simd_three_different(u32 w, Instruction& in) {
    const u32 a = bits(w, 11, 8), size = bits(w, 21, 20);
    const bool u = bit(w, 24);
    const u32 d = d_num(w, 15, 12, 22), n = d_num(w, 19, 16, 7), m = d_num(w, 3, 0, 5);
    const Dt sdt = u ? kU[size] : kS[size];
    const auto long3 = [&](Op op, Dt dt) {  // q, d, d
        if (d & 1) return false;
        set(in, op, {qreg(d >> 1), dreg(n), dreg(m)});
        in.dt = dt;
        return true;
    };
    switch (a) {
        case 0: return long3(Op::Vaddl, sdt);
        case 1:
            if ((d | n) & 1) return false;
            set(in, Op::Vaddw, {qreg(d >> 1), qreg(n >> 1), dreg(m)});
            in.dt = sdt;
            return true;
        case 2: return long3(Op::Vsubl, sdt);
        case 3:
            if ((d | n) & 1) return false;
            set(in, Op::Vsubw, {qreg(d >> 1), qreg(n >> 1), dreg(m)});
            in.dt = sdt;
            return true;
        case 4:
        case 6: {
            if ((n | m) & 1) return false;
            const Op op = a == 4 ? (u ? Op::Vraddhn : Op::Vaddhn) : (u ? Op::Vrsubhn : Op::Vsubhn);
            set(in, op, {dreg(d), qreg(n >> 1), qreg(m >> 1)});
            in.dt = kI[size + 1];
            return true;
        }
        case 5: return long3(Op::Vabal, sdt);
        case 7: return long3(Op::Vabdl, sdt);
        case 8: return long3(Op::Vmlal, sdt);
        case 10: return long3(Op::Vmlsl, sdt);
        case 9:
        case 11:
        case 13:
            if (u || size == 0) return false;
            return long3(a == 9 ? Op::Vqdmlal : a == 11 ? Op::Vqdmlsl : Op::Vqdmull, kS[size]);
        case 12: return long3(Op::Vmull, sdt);
        case 14:
            if (u || size != 0) return false;
            return long3(Op::Vmull, Dt::P8);
        default: return false;
    }
}

// Two registers and a scalar (A7.4.3).
bool simd_scalar(u32 w, Instruction& in) {
    const u32 a = bits(w, 11, 8), size = bits(w, 21, 20);
    const bool q = bit(w, 24);
    const bool f = bit(w, 8) && (a == 1 || a == 5 || a == 9);
    const u32 d = d_num(w, 15, 12, 22), n = d_num(w, 19, 16, 7);
    if (size == 0 || size == 3) return false;
    const u32 m = size == 1 ? bits(w, 2, 0) : bits(w, 3, 0);
    const u32 index = size == 1 ? (bit(w, 5) << 1) | bit(w, 3) : bit(w, 5);
    const Operand scalar = dscalar(m, index);
    const auto same = [&](Op op, Dt dt) {  // q/d, q/d, scalar
        if (q && ((d | n) & 1)) return false;
        set(in, op, {vreg(q, d), vreg(q, n), scalar});
        in.dt = dt;
        return true;
    };
    const auto wide = [&](Op op, Dt dt) {
        if (d & 1) return false;
        set(in, op, {qreg(d >> 1), dreg(n), scalar});
        in.dt = dt;
        return true;
    };
    const Dt idt = f ? Dt::F32 : kI[size];
    if (f && size != 2) return false;
    switch (a) {
        case 0: case 1: return same(Op::Vmla, idt);
        case 4: case 5: return same(Op::Vmls, idt);
        case 8: case 9: return same(Op::Vmul, idt);
        case 2: return wide(Op::Vmlal, q ? kU[size] : kS[size]);
        case 6: return wide(Op::Vmlsl, q ? kU[size] : kS[size]);
        case 10: return wide(Op::Vmull, q ? kU[size] : kS[size]);
        case 3: if (q) return false; return wide(Op::Vqdmlal, kS[size]);
        case 7: if (q) return false; return wide(Op::Vqdmlsl, kS[size]);
        case 11: if (q) return false; return wide(Op::Vqdmull, kS[size]);
        case 12: return same(Op::Vqdmulh, kS[size]);
        case 13: return same(Op::Vqrdmulh, kS[size]);
        default: return false;
    }
}

// Two registers and a shift amount (A7.4.4).
bool simd_shift(u32 w, Instruction& in) {
    const u32 a = bits(w, 11, 8), imm6 = bits(w, 21, 16);
    const bool u = bit(w, 24), l = bit(w, 7), q = bit(w, 6);
    const bool op6 = bit(w, 6);  // (narrowing shifts: rounding / saturating variant)
    const u32 d = d_num(w, 15, 12, 22), m = d_num(w, 3, 0, 5);
    u32 esize;
    if (l) esize = 64;
    else if (imm6 & 0x20) esize = 32;
    else if (imm6 & 0x10) esize = 16;
    else if (imm6 & 0x08) esize = 8;
    else return false;
    const u32 size_index = esize == 8 ? 0 : esize == 16 ? 1 : esize == 32 ? 2 : 3;
    const u32 right = (l ? 64 : 2 * esize) - imm6;  // shift amount of right shifts
    const u32 left = l ? imm6 : imm6 - esize;       // of left shifts
    const auto same = [&](Op op, Dt dt, u32 amount) {
        if (q && ((d | m) & 1)) return false;
        set(in, op, {vreg(q, d), vreg(q, m), imm(amount)});
        in.dt = dt;
        return true;
    };
    const Dt sdt = u ? kU[size_index] : kS[size_index];
    switch (a) {
        case 0: return same(Op::Vshr, sdt, right);
        case 1: return same(Op::Vsra, sdt, right);
        case 2: return same(Op::Vrshr, sdt, right);
        case 3: return same(Op::Vrsra, sdt, right);
        case 4:
            if (!u) return false;
            return same(Op::Vsri, kB[size_index], right);
        case 5: return same(u ? Op::Vsli : Op::Vshl, u ? kB[size_index] : kI[size_index], left);
        case 6:
            if (!u) return false;
            return same(Op::Vqshlu, kS[size_index], left);
        case 7: return same(Op::Vqshl, sdt, left);
        case 8:
        case 9: {
            // Narrowing shifts: the immediate encodes the element size of the result (imm6 = 001xxx: 16-bit
            // source, ...); the shift is (2 * half) - imm6 where half is the narrow element size.
            if (l) return false;
            u32 half;
            if (imm6 & 0x20) half = 32;
            else if (imm6 & 0x10) half = 16;
            else if (imm6 & 0x08) half = 8;
            else return false;
            const u32 amount = 2 * half - imm6;
            const u32 hi = half == 8 ? 1 : half == 16 ? 2 : 3;  // the wide source element size index
            Op op;
            Dt dt;
            if (a == 8) {
                if (!u) {
                    op = op6 ? Op::Vrshrn : Op::Vshrn;
                    dt = kI[hi];
                } else {
                    op = op6 ? Op::Vqrshrun : Op::Vqshrun;
                    dt = kS[hi];
                }
            } else {
                op = op6 ? Op::Vqrshrn : Op::Vqshrn;
                dt = u ? kU[hi] : kS[hi];
            }
            if (m & 1) return false;
            set(in, op, {dreg(d), qreg(m >> 1), imm(amount)});
            in.dt = dt;
            return true;
        }
        case 10: {  // vshll / vmovl
            if (q || l || (d & 1)) return false;
            if (left == 0) {
                set(in, Op::Vmovl, {qreg(d >> 1), dreg(m)});
                in.dt = sdt;
                return true;
            }
            set(in, Op::Vshll, {qreg(d >> 1), dreg(m), imm(left)});
            in.dt = sdt;
            return true;
        }
        case 14:
        case 15: {  // vcvt between fixed point and floating point
            if (l || !(imm6 & 0x20)) return false;
            const bool to_fixed = bit(w, 8);
            if (q && ((d | m) & 1)) return false;
            set(in, Op::Vcvt, {vreg(q, d), vreg(q, m), imm(64 - imm6)});
            in.dt = to_fixed ? (u ? Dt::U32 : Dt::S32) : Dt::F32;
            in.dt2 = to_fixed ? Dt::F32 : (u ? Dt::U32 : Dt::S32);
            return true;
        }
        default: return false;
    }
}

// Two registers, miscellaneous (A7.4.5).
bool simd_misc(u32 w, Instruction& in) {
    const u32 a = bits(w, 17, 16), b = bits(w, 10, 6), size = bits(w, 19, 18);
    const bool q = bit(w, 6);
    const u32 d = d_num(w, 15, 12, 22), m = d_num(w, 3, 0, 5);
    const auto two = [&](Op op, Dt dt) {
        if (q && ((d | m) & 1)) return false;
        set(in, op, {vreg(q, d), vreg(q, m)});
        in.dt = dt;
        return true;
    };
    const auto zero = [&](Op op, Dt dt) {
        if (q && ((d | m) & 1)) return false;
        set(in, op, {vreg(q, d), vreg(q, m), imm(0)});
        in.dt = dt;
        return true;
    };
    switch (a) {
        case 0:
            switch (b >> 1) {
                case 0: if (size == 3) return false; return two(Op::Vrev64, kB[size]);
                case 1: if (size >= 2) return false; return two(Op::Vrev32, kB[size]);
                case 2: if (size >= 1) return false; return two(Op::Vrev16, kB[size]);
                case 4: case 5: if (size == 3) return false; return two(Op::Vpaddl, bit(w, 7) ? kU[size] : kS[size]);
                case 8: if (size == 3) return false; return two(Op::Vcls, kS[size]);
                case 9: if (size == 3) return false; return two(Op::Vclz, kI[size]);
                case 10: if (size != 0) return false; return two(Op::Vcnt, Dt::B8);
                case 11: if (size != 0) return false; return two(Op::Vmvn, Dt::None);
                case 12: case 13: if (size == 3) return false; return two(Op::Vpadal, bit(w, 7) ? kU[size] : kS[size]);
                case 14: if (size == 3) return false; return two(Op::Vqabs, kS[size]);
                case 15: if (size == 3) return false; return two(Op::Vqneg, kS[size]);
                default: return false;
            }
        case 1: {
            const bool f = bit(w, 10);
            if (size == 3 || (f && size != 2)) return false;
            const Dt sdt = f ? Dt::F32 : kS[size];
            const Dt idt = f ? Dt::F32 : kI[size];
            switch ((b >> 1) & 7) {
                case 0: return zero(Op::Vcgt, sdt);
                case 1: return zero(Op::Vcge, sdt);
                case 2: return zero(Op::Vceq, idt);
                case 3: return zero(Op::Vcle, sdt);
                case 4: return zero(Op::Vclt, sdt);
                case 6: return two(Op::Vabs, sdt);
                case 7: return two(Op::Vneg, sdt);
                default: return false;
            }
        }
        case 2:
            switch (b >> 1) {
                case 0: if (size != 0) return false; return two(Op::Vswp, Dt::None);
                case 1: if (size == 3) return false; return two(Op::Vtrn, kB[size]);
                case 2: if (size == 3 || (!q && size == 2)) return false; return two(Op::Vuzp, kB[size]);
                case 3: if (size == 3 || (!q && size == 2)) return false; return two(Op::Vzip, kB[size]);
                case 4: {
                    if (size == 3 || (m & 1)) return false;
                    const Op op = bit(w, 6) ? Op::Vqmovun : Op::Vmovn;
                    set(in, op, {dreg(d), qreg(m >> 1)});
                    in.dt = bit(w, 6) ? kS[size + 1] : kI[size + 1];
                    return true;
                }
                case 5: {
                    if (size == 3 || (m & 1)) return false;
                    set(in, Op::Vqmovn, {dreg(d), qreg(m >> 1)});
                    in.dt = bit(w, 6) ? kU[size + 1] : kS[size + 1];
                    return true;
                }
                case 6:
                    if (bit(w, 6) || size == 3 || (d & 1)) return false;
                    set(in, Op::Vshll, {qreg(d >> 1), dreg(m), imm(8u << size)});
                    in.dt = kI[size];
                    return true;
                case 12:
                case 14: {  // vcvt between half and single precision
                    if (size != 1 || bit(w, 6)) return false;
                    const bool to_single = bit(w, 8);
                    if (to_single) {
                        if (d & 1) return false;
                        set(in, Op::Vcvt, {qreg(d >> 1), dreg(m)});
                        in.dt = Dt::F32;
                        in.dt2 = Dt::F16;
                    } else {
                        if (m & 1) return false;
                        set(in, Op::Vcvt, {dreg(d), qreg(m >> 1)});
                        in.dt = Dt::F16;
                        in.dt2 = Dt::F32;
                    }
                    return true;
                }
                default: return false;
            }
        default: {
            if (size != 2) return false;
            const bool f = bit(w, 8);
            if ((b & 0x1A) == 0x10) return two(Op::Vrecpe, f ? Dt::F32 : Dt::U32);
            if ((b & 0x1A) == 0x12) return two(Op::Vrsqrte, f ? Dt::F32 : Dt::U32);
            if ((b & 0x18) == 0x18) {
                const u32 op = bits(w, 8, 7);  // 00 f32.s32, 01 f32.u32, 10 s32.f32, 11 u32.f32
                if (!two(Op::Vcvt, Dt::None)) return false;
                in.dt = op < 2 ? Dt::F32 : (op == 2 ? Dt::S32 : Dt::U32);
                in.dt2 = op < 2 ? (op == 0 ? Dt::S32 : Dt::U32) : Dt::F32;
                return true;
            }
            return false;
        }
    }
}

// One register and a modified immediate (A7.4.6).
bool simd_immediate(u32 w, Instruction& in) {
    const u32 cmode = bits(w, 11, 8);
    const bool op = bit(w, 5), q = bit(w, 6);
    const u32 d = d_num(w, 15, 12, 22);
    if (q && (d & 1)) return false;
    Operand v{OperandKind::SimdImm};
    v.index = static_cast<u8>((bit(w, 24) << 7) | (bits(w, 18, 16) << 4) | bits(w, 3, 0));
    v.amount = static_cast<u8>(cmode);
    v.count = op ? 1 : 0;
    Op o;
    Dt dt;
    if ((cmode & 8) == 0) {  // 0xxx: 32-bit
        o = (cmode & 1) ? (op ? Op::Vbic : Op::Vorr) : (op ? Op::Vmvn : Op::Vmov);
        dt = Dt::I32;
    } else if ((cmode & 0xC) == 8) {  // 10xx: 16-bit
        o = (cmode & 1) ? (op ? Op::Vbic : Op::Vorr) : (op ? Op::Vmvn : Op::Vmov);
        dt = Dt::I16;
    } else if ((cmode & 0xE) == 0xC) {  // 110x
        o = op ? Op::Vmvn : Op::Vmov;
        dt = Dt::I32;
    } else if (cmode == 0xE) {
        o = Op::Vmov;
        dt = op ? Dt::I64 : Dt::I8;
    } else {
        if (op) return false;
        o = Op::Vmov;
        dt = Dt::F32;
    }
    set(in, o, {vreg(q, d), v});
    in.dt = dt;
    return true;
}

}  // namespace

bool decode_vfp(u32 w, Instruction& in) {
    if (bits(w, 11, 9) != 5 || bit(w, 4)) return false;
    return vfp_data(w, in);
}

bool decode_vfp_transfer(u32 w, Instruction& in) {
    if (bits(w, 11, 9) != 5) return false;
    if (bits(w, 27, 25) == 6) return vfp_load_store(w, in);
    if (bits(w, 27, 24) == 0xE && bit(w, 4)) return vfp_core_transfer(w, in);
    return false;
}

bool decode_simd_data(u32 w, Instruction& in) {
    if ((w & 0xFE000000u) != 0xF2000000u) return false;
    in.cond = in.mode == Mode::Arm ? Cond::None : in.cond;
    const u32 a = bits(w, 23, 19), b = bits(w, 11, 8), c = bits(w, 7, 4);
    const bool u = bit(w, 24);
    if ((a & 0x10) == 0) return simd_three_same(w, in);
    if ((a & 0x17) == 0x10 && (c & 9) == 1) return simd_immediate(w, in);
    if ((c & 1) == 1) return simd_shift(w, in);  // (1x001, 1x01x, 1x1xx with c = xxx1; and L = 1)
    if ((a & 0x16) != 0x16) {
        if ((c & 5) == 0) return simd_three_different(w, in);
        if ((c & 5) == 4) return simd_scalar(w, in);
        return false;
    }
    // 1x11x
    if (!u) {  // vext
        const Regs r = regs3(w);
        const u32 imm4 = bits(w, 11, 8);
        if (r.q && ((r.d | r.n | r.m) & 1)) return false;
        if (!r.q && (imm4 & 8)) return false;
        // (one encoding for every element size: the reference prints the largest size the byte index allows)
        u32 index = imm4;
        Dt dt = Dt::B8;
        if (r.q && imm4 % 8 == 0) dt = Dt::B64, index = imm4 / 8;
        else if (imm4 % 4 == 0) dt = Dt::B32, index = imm4 / 4;
        else if (imm4 % 2 == 0) dt = Dt::B16, index = imm4 / 2;
        set(in, Op::Vext, {vreg(r.q, r.d), vreg(r.q, r.n), vreg(r.q, r.m), imm(index)});
        in.dt = dt;
        return true;
    }
    if ((b & 8) == 0) return simd_misc(w, in);
    if ((b & 0xC) == 8) {  // vtbl / vtbx
        const u32 d = d_num(w, 15, 12, 22), n = d_num(w, 19, 16, 7), m = d_num(w, 3, 0, 5);
        const u32 len = bits(w, 9, 8) + 1;
        if (n + len > 32) return false;
        set(in, bit(w, 6) ? Op::Vtbx : Op::Vtbl, {dreg(d), dlist(n, len), dreg(m)});
        in.dt = Dt::B8;
        return true;
    }
    if (b == 0xC && (c & 9) == 0) {  // vdup (scalar)
        const u32 imm4 = bits(w, 19, 16);
        const bool q = bit(w, 6);
        const u32 d = d_num(w, 15, 12, 22), m = d_num(w, 3, 0, 5);
        Dt dt;
        u32 index;
        if (imm4 & 1) dt = Dt::B8, index = imm4 >> 1;
        else if (imm4 & 2) dt = Dt::B16, index = imm4 >> 2;
        else if (imm4 & 4) dt = Dt::B32, index = imm4 >> 3;
        else return false;
        if (q && (d & 1)) return false;
        set(in, Op::Vdup, {vreg(q, d), dscalar(m, index)});
        in.dt = dt;
        return true;
    }
    return false;
}

// --- Advanced SIMD element and structure loads and stores (A7.7) ---

bool decode_simd_memory(u32 w, Instruction& in) {
    if ((w & 0xFF100000u) != 0xF4000000u) return false;
    if (in.mode == Mode::Arm) in.cond = Cond::None;
    const bool all_types = bit(w, 23), load = bit(w, 21);
    const u8 rn = static_cast<u8>(bits(w, 19, 16)), rm = static_cast<u8>(bits(w, 3, 0));
    const u32 d = d_num(w, 15, 12, 22), type = bits(w, 11, 8), size = bits(w, 7, 6);
    Operand m{OperandKind::Mem, rn};
    if (rm == kSp) {
        m.flags = kMemPre | kMemWriteBack;
    } else if (rm != kPc) {
        m.flags = kMemPostReg;
        m.reg2 = rm;
    }
    if (!all_types) {  // multiple elements
        u32 regs, spacing = 1, n;
        switch (type) {
            case 2: n = 1, regs = 4; break;
            case 6: n = 1, regs = 3; break;
            case 7: n = 1, regs = 1; break;
            case 10: n = 1, regs = 2; break;
            case 3: n = 2, regs = 2, spacing = 2; break;
            case 8: n = 2, regs = 1; break;
            case 9: n = 2, regs = 1, spacing = 2; break;
            case 4: n = 3, regs = 1; break;
            case 5: n = 3, regs = 1, spacing = 2; break;
            case 0: n = 4, regs = 1; break;
            case 1: n = 4, regs = 1, spacing = 2; break;
            default: return false;
        }
        const u32 align = bits(w, 5, 4);
        if (n == 1) {
            if ((regs == 1 && (align & 2)) || (regs == 3 && (align & 2)) || (regs == 2 && align == 3)) return false;
        } else if (n == 2) {
            if (size == 3 || (regs == 1 && align == 3)) return false;
        } else if (n == 3) {
            if (size == 3 || (align & 2)) return false;
        } else {
            if (size == 3) return false;
        }
        if (align) m.count = static_cast<u8>(4u << align);
        static constexpr Op kLoad[4] = {Op::Vld1, Op::Vld2, Op::Vld3, Op::Vld4};
        static constexpr Op kStore[4] = {Op::Vst1, Op::Vst2, Op::Vst3, Op::Vst4};
        const u32 count = n == 1 ? regs : (n == 2 && regs == 2 ? 4 : n);
        const u32 step = n == 2 && regs == 2 ? 1 : spacing;
        set(in, load ? kLoad[n - 1] : kStore[n - 1], {dlist(d, count, step), m});
        in.dt = n == 1 ? kB[size] : kB[size];
        if (d + (count - 1) * step > 31) {
            if (n <= 2) return false;
            unpredictable(in);
        }
        return true;
    }
    // single element to one lane (size != 3) or to all lanes (size == 3 field in bits 11-10)
    const u32 n = bits(w, 9, 8) + 1;
    static constexpr Op kLoad[4] = {Op::Vld1, Op::Vld2, Op::Vld3, Op::Vld4};
    static constexpr Op kStore[4] = {Op::Vst1, Op::Vst2, Op::Vst3, Op::Vst4};
    const u32 lane_size = bits(w, 11, 10);
    Operand list{OperandKind::DLaneList, static_cast<u8>(d)};
    list.count = static_cast<u8>(n);
    list.amount = 1;
    if (lane_size == 3) {  // all lanes
        if (!load) return false;
        const u32 sz = bits(w, 7, 6), t = bit(w, 5), a = bit(w, 4);
        list.flags = kAllLanes;
        u32 count = n;
        if (n == 1) {
            if (sz == 3 || (sz == 0 && a)) return false;
            count = t ? 2 : 1;
            list.amount = 1;
            if (a) m.count = static_cast<u8>(1u << sz);
        } else {
            list.amount = static_cast<u8>(t ? 2 : 1);
            if (n == 2) {
                if (sz == 3) return false;
                if (a) m.count = static_cast<u8>(2u << sz);
            } else if (n == 3) {
                if (sz == 3 || a) return false;
            } else {
                if (sz == 3 && !a) return false;
                if (a) m.count = static_cast<u8>(sz == 2 ? 8 : sz == 3 ? 16 : 4u << sz);
            }
        }
        list.count = static_cast<u8>(count);
        if (d + (count - 1) * list.amount > 31) {
            if (n <= 2) return false;
            unpredictable(in);
        }
        set(in, kLoad[n - 1], {list, m});
        in.dt = sz == 3 ? Dt::B32 : kB[sz];
        return true;
    }
    const u32 ia = bits(w, 7, 4);
    u32 index, spacing = 1;
    switch (lane_size) {
        case 0: index = ia >> 1; break;
        case 1: index = ia >> 2; spacing = (ia & 2) ? 2 : 1; break;
        default: index = ia >> 3; spacing = (ia & 4) ? 2 : 1; break;
    }
    if (n == 1) spacing = 1;
    list.amount = static_cast<u8>(spacing);
    list.index = static_cast<u8>(index);
    // alignment
    if (n == 1) {
        if (lane_size == 0 && (ia & 1)) return false;
        if (lane_size == 1 && (ia & 2)) return false;
        if (lane_size == 1 && (ia & 1)) m.count = 2;
        if (lane_size == 2 && (ia & 4)) return false;
        if (lane_size == 2 && (ia & 3) == 3) m.count = 4;
        else if (lane_size == 2 && (ia & 3) != 0) return false;
    } else if (n == 2) {
        if (lane_size == 0 && (ia & 1)) m.count = 2;
        if (lane_size == 1 && (ia & 1)) m.count = 4;
        if (lane_size == 2 && (ia & 2)) return false;
        if (lane_size == 2 && (ia & 1)) m.count = 8;
    } else if (n == 3) {
        if (lane_size == 0 && (ia & 1)) return false;
        if (lane_size == 1 && (ia & 1)) return false;
        if (lane_size == 2 && (ia & 3)) return false;
    } else {
        if (lane_size == 0 && (ia & 1)) m.count = 4;
        if (lane_size == 1 && (ia & 1)) m.count = 8;
        if (lane_size == 2 && (ia & 3) == 3) return false;
        if (lane_size == 2 && (ia & 3)) m.count = static_cast<u8>(8u << (ia & 3) >> 1);
    }
    if (d + (n - 1) * spacing > 31) return false;
    set(in, load ? kLoad[n - 1] : kStore[n - 1], {list, m});
    in.dt = kB[lane_size];
    return true;
}

}  // namespace psvrecore::arm
