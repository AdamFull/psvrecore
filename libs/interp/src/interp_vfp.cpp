// Floating-point instructions (VFPv3, A8.8: VADD...VSTR), on the integer soft float (softfloat.cpp).

#include "exec.hpp"
#include "softfloat.hpp"

namespace psvrecore::interp::detail {

namespace {

using arm::Dt;

bool is_s(const Operand& o) { return o.kind == OperandKind::SReg; }
bool is_d(const Operand& o) { return o.kind == OperandKind::DReg; }

bool vfp_form(const Instruction& in) {
    if (in.dt == Dt::F64 || in.dt2 == Dt::F64) return true;
    for (u8 i = 0; i < in.operand_count; ++i)
        if (is_s(in.operands[i])) return true;
    return false;
}

// The FPSCR bits an FPSCR write keeps, and the settings this implementation (like the code generator) refuses.
constexpr u32 kWritable = 0xFFF7009Fu, kUnsupported = 0x04F70000u;

struct Vfp {
    Exec& x;
    u64 get(const Operand& o) const { return is_s(o) ? x.ctx.vfp.s[o.reg] : x.ctx.vfp.d[o.reg]; }
    void set(const Operand& o, u64 v) const {
        if (is_s(o)) x.ctx.vfp.s[o.reg] = static_cast<u32>(v);
        else x.ctx.vfp.d[o.reg] = v;
    }
    u32 fpscr() const { return x.ctx.fpscr; }
};

bool arithmetic(Exec& x, const Instruction& in) {
    const Vfp v{x};
    const int n = in.dt == Dt::F64 ? 64 : 32;
    const u32 fpscr = x.ctx.fpscr;
    const auto& d = in.operands[0];
    switch (in.op) {
        case Op::Vadd: v.set(d, fp::add(v.get(in.operands[1]), v.get(in.operands[2]), n, fpscr)); return true;
        case Op::Vsub: v.set(d, fp::sub(v.get(in.operands[1]), v.get(in.operands[2]), n, fpscr)); return true;
        case Op::Vmul: v.set(d, fp::mul(v.get(in.operands[1]), v.get(in.operands[2]), n, fpscr)); return true;
        case Op::Vdiv: v.set(d, fp::div(v.get(in.operands[1]), v.get(in.operands[2]), n, fpscr)); return true;
        case Op::Vnmul: v.set(d, fp::neg(fp::mul(v.get(in.operands[1]), v.get(in.operands[2]), n, fpscr), n)); return true;
        case Op::Vmla:
        case Op::Vmls:
        case Op::Vnmla:
        case Op::Vnmls: {
            // VMLA: d + n*m; VMLS: d + -(n*m); VNMLA: -d + -(n*m); VNMLS: -d + n*m. The product is rounded first.
            u64 product = fp::mul(v.get(in.operands[1]), v.get(in.operands[2]), n, fpscr);
            if (in.op == Op::Vmls || in.op == Op::Vnmla) product = fp::neg(product, n);
            u64 acc = v.get(d);
            if (in.op == Op::Vnmla || in.op == Op::Vnmls) acc = fp::neg(acc, n);
            v.set(d, fp::add(acc, product, n, fpscr));
            return true;
        }
        case Op::Vabs: v.set(d, fp::abs(v.get(in.operands[1]), n)); return true;
        case Op::Vneg: v.set(d, fp::neg(v.get(in.operands[1]), n)); return true;
        case Op::Vsqrt: v.set(d, fp::sqrt(v.get(in.operands[1]), n, fpscr)); return true;
        case Op::Vcmp:
        case Op::Vcmpe: {
            const u64 b = in.operands[1].kind == OperandKind::Imm ? 0 : v.get(in.operands[1]);
            const u32 nzcv = fp::compare(v.get(d), b, n, fpscr);
            x.ctx.fpscr = (x.ctx.fpscr & 0x0FFFFFFFu) | (nzcv << 28);
            return true;
        }
        default: return false;
    }
}

bool convert(Exec& x, const Instruction& in) {
    const Vfp v{x};
    const u32 fpscr = x.ctx.fpscr;
    const auto& d = in.operands[0];
    const auto& m = in.operands[1];
    switch (in.op) {
        case Op::Vcvtb:
        case Op::Vcvtt: {
            const u32 shift = in.op == Op::Vcvtt ? 16 : 0;
            if (in.dt == Dt::F16) {
                const u32 h = static_cast<u32>(fp::convert(v.get(m), 32, 16, fpscr));
                x.ctx.vfp.s[d.reg] = (x.ctx.vfp.s[d.reg] & ~(0xFFFFu << shift)) | (h << shift);
            } else {
                x.ctx.vfp.s[d.reg] = static_cast<u32>(fp::convert((v.get(m) >> shift) & 0xFFFF, 16, 32, fpscr));
            }
            return true;
        }
        case Op::Vcvt:
        case Op::Vcvtr: break;
        default: return false;
    }
    const auto fixed = [](Dt dt) { return dt == Dt::S16 || dt == Dt::U16 || dt == Dt::S32 || dt == Dt::U32; };
    const auto bits = [](Dt dt) { return dt == Dt::S16 || dt == Dt::U16 ? 16 : 32; };
    const auto is_unsigned = [](Dt dt) { return dt == Dt::U16 || dt == Dt::U32; };
    if (in.operand_count == 3) {  // fixed point
        const int fraction = static_cast<int>(in.operands[2].imm);
        if (fixed(in.dt)) {
            const int n = in.dt2 == Dt::F64 ? 64 : 32;
            const u64 r = fp::to_fixed(v.get(m), n, bits(in.dt), fraction, is_unsigned(in.dt), true, fpscr);
            v.set(d, n == 64 ? r : static_cast<u32>(r));
        } else {
            const int n = in.dt == Dt::F64 ? 64 : 32;
            v.set(d, fp::from_fixed(v.get(m), bits(in.dt2), fraction, is_unsigned(in.dt2), n, false, fpscr));
        }
        return true;
    }
    if ((in.dt == Dt::F64 && in.dt2 == Dt::F32) || (in.dt == Dt::F32 && in.dt2 == Dt::F64)) {
        const int from = in.dt2 == Dt::F64 ? 64 : 32, to = in.dt == Dt::F64 ? 64 : 32;
        v.set(d, fp::convert(v.get(m), from, to, fpscr));
        return true;
    }
    if (in.dt2 == Dt::S32 || in.dt2 == Dt::U32) {  // integer to floating point: the FPSCR's rounding
        const int n = in.dt == Dt::F64 ? 64 : 32;
        v.set(d, fp::from_fixed(v.get(m), 32, 0, in.dt2 == Dt::U32, n, false, fpscr));
        return true;
    }
    if (in.dt == Dt::S32 || in.dt == Dt::U32) {  // floating point to integer: toward zero, or (vcvtr) the FPSCR's
        const int n = in.dt2 == Dt::F64 ? 64 : 32;
        v.set(d, static_cast<u32>(fp::to_fixed(v.get(m), n, 32, 0, in.dt == Dt::U32, in.op == Op::Vcvt, fpscr)));
        return true;
    }
    return false;
}

bool moves(Exec& x, const Instruction& in) {
    if (in.op != Op::Vmov) return false;
    const Vfp v{x};
    const auto& a = in.operands[0];
    const auto& b = in.operands[1];
    auto& vfp = x.ctx.vfp;
    if (b.kind == OperandKind::FloatImm) {
        // VFPExpandImm: the single-precision value, or the same value in double precision.
        if (is_s(a)) vfp.s[a.reg] = b.imm;
        else vfp.d[a.reg] = fp::convert(b.imm, 32, 64, 0);
        return true;
    }
    if (in.operand_count == 2 && (is_s(a) || is_d(a)) && a.kind == b.kind) {
        v.set(a, v.get(b));
        return true;
    }
    if (in.operand_count == 2 && a.kind == OperandKind::Reg && is_s(b)) {
        x.ctx.r[a.reg] = vfp.s[b.reg];
        return true;
    }
    if (in.operand_count == 2 && is_s(a) && b.kind == OperandKind::Reg) {
        vfp.s[a.reg] = x.reg(b.reg);
        return true;
    }
    if (in.operand_count == 4 && a.kind == OperandKind::Reg) {
        const u32 lo = vfp.s[in.operands[2].reg], hi = vfp.s[in.operands[3].reg];
        x.ctx.r[a.reg] = lo;
        x.ctx.r[b.reg] = hi;
        return true;
    }
    if (in.operand_count == 4 && is_s(a)) {
        const u32 lo = x.reg(in.operands[2].reg), hi = x.reg(in.operands[3].reg);
        vfp.s[a.reg] = lo;
        vfp.s[b.reg] = hi;
        return true;
    }
    if (in.operand_count == 3 && a.kind == OperandKind::Reg) {
        const u64 value = vfp.d[in.operands[2].reg];
        x.ctx.r[a.reg] = static_cast<u32>(value);
        x.ctx.r[b.reg] = static_cast<u32>(value >> 32);
        return true;
    }
    if (in.operand_count == 3 && is_d(a)) {
        vfp.d[a.reg] = x.reg(b.reg) | (static_cast<u64>(x.reg(in.operands[2].reg)) << 32);
        return true;
    }
    // Scalars: elements of a D register by index (little-endian lanes).
    const auto element_bits = [&](Dt dt) { return dt == Dt::S8 || dt == Dt::U8 || dt == Dt::B8 ? 8 : dt == Dt::S16 || dt == Dt::U16 || dt == Dt::B16 ? 16 : 32; };
    if (a.kind == OperandKind::Reg && b.kind == OperandKind::DScalar) {
        const int esize = element_bits(in.dt);
        const u64 e = (vfp.d[b.reg] >> (esize * b.index)) & ((u64{1} << esize) - 1);
        u32 value = static_cast<u32>(e);
        if (in.dt == Dt::S8) value = static_cast<u32>(static_cast<i8>(e));
        if (in.dt == Dt::S16) value = static_cast<u32>(static_cast<i16>(e));
        x.ctx.r[a.reg] = value;
        return true;
    }
    if (a.kind == OperandKind::DScalar && b.kind == OperandKind::Reg) {
        const int esize = element_bits(in.dt);
        const u64 field = ((u64{1} << esize) - 1) << (esize * a.index);
        vfp.d[a.reg] = (vfp.d[a.reg] & ~field) | ((static_cast<u64>(x.reg(b.reg)) << (esize * a.index)) & field);
        return true;
    }
    return false;
}

bool load_store(Exec& x, const Instruction& in) {
    auto& vfp = x.ctx.vfp;
    switch (in.op) {
        case Op::Vldr:
        case Op::Vstr: {
            const auto& r = in.operands[0];
            const u32 address = x.address(in.operands[1]).address;
            if (in.op == Op::Vldr) {
                if (is_d(r)) vfp.d[r.reg] = x.mem(address, 4) | (static_cast<u64>(x.mem(address + 4, 4)) << 32);
                else vfp.s[r.reg] = x.mem(address, 4);
            } else if (is_d(r)) {
                x.mem_write(address, 4, static_cast<u32>(vfp.d[r.reg]));
                x.mem_write(address + 4, 4, static_cast<u32>(vfp.d[r.reg] >> 32));
            } else {
                x.mem_write(address, 4, vfp.s[r.reg]);
            }
            return true;
        }
        case Op::Vldmia: case Op::Vldmdb: case Op::Vstmia: case Op::Vstmdb: case Op::Vpush: case Op::Vpop: {
            const bool load = in.op == Op::Vldmia || in.op == Op::Vldmdb || in.op == Op::Vpop;
            const bool add = in.op == Op::Vldmia || in.op == Op::Vstmia || in.op == Op::Vpop;
            const bool push_pop = in.op == Op::Vpush || in.op == Op::Vpop;
            const u32 n = push_pop ? arm::kSp : in.operands[0].reg;
            const Operand& list = in.operands[push_pop ? 0 : 1];
            const bool wback = push_pop || (in.flags & arm::kWriteBack);
            const bool single = list.kind == OperandKind::SRegList;
            // imm32 = imm8 * 4: the registers' words, plus one for FLDMX/FSTMX.
            const u32 imm32 = (single ? 4u : 8u) * list.count + ((in.flags & arm::kXForm) ? 4u : 0u);
            const u32 base = x.reg(n);
            u32 address = add ? base : base - imm32;
            if (wback) x.ctx.r[n] = add ? base + imm32 : base - imm32;
            for (u32 i = 0; i < list.count; ++i) {
                const u32 r = list.reg + i;
                if (single) {
                    if (load) vfp.s[r] = x.mem(address, 4);
                    else x.mem_write(address, 4, vfp.s[r]);
                    address += 4;
                } else {
                    if (load) {
                        vfp.d[r] = x.mem(address, 4) | (static_cast<u64>(x.mem(address + 4, 4)) << 32);
                    } else {
                        x.mem_write(address, 4, static_cast<u32>(vfp.d[r]));
                        x.mem_write(address + 4, 4, static_cast<u32>(vfp.d[r] >> 32));
                    }
                    address += 8;
                }
            }
            return true;
        }
        default: return false;
    }
}

bool status(Exec& x, const Instruction& in) {
    constexpr u32 kFpscr = 5, kApsrNzcv = 11;
    if (in.op == Op::Vmrs) {
        if (in.operands[1].imm != kFpscr) return x.trap(PSV_TRAP_PRIVILEGED);
        if (in.operands[0].kind == OperandKind::Sysreg && in.operands[0].imm == kApsrNzcv) {
            x.ctx.n = x.ctx.fpscr >> 31;
            x.ctx.z = (x.ctx.fpscr >> 30) & 1;
            x.ctx.c = (x.ctx.fpscr >> 29) & 1;
            x.ctx.v = (x.ctx.fpscr >> 28) & 1;
        } else {
            x.ctx.r[in.operands[0].reg] = x.ctx.fpscr;
        }
        return true;
    }
    if (in.op == Op::Vmsr) {
        if (in.operands[0].imm != kFpscr) return x.trap(PSV_TRAP_PRIVILEGED);
        const u32 value = x.reg(in.operands[1].reg);
        if (value & kUnsupported) return x.trap(PSV_TRAP_FPSCR);
        x.ctx.fpscr = value & kWritable;
        return true;
    }
    return false;
}

}  // namespace

bool exec_vfp(Exec& x) {
    const Instruction& in = x.in;
    if (status(x, in) || load_store(x, in)) return true;
    if (in.op == Op::Vmov && moves(x, in)) return true;
    if (!vfp_form(in)) return false;
    return arithmetic(x, in) || convert(x, in);
}

}  // namespace psvrecore::interp::detail
