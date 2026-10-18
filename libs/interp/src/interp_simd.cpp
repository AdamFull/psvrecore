// Advanced SIMD instructions (A8.8, VABA...VZIP), element by element after the pseudocode. Floating-point elements
// go through the soft float with the standard FPSCR value.

#include <bit>

#include "exec.hpp"
#include "softfloat.hpp"

namespace psvrecore::interp::detail {

namespace {

using arm::Dt;
using i128 = __int128;

u32 esize_of(Dt dt) {
    switch (dt) {
        case Dt::S8: case Dt::U8: case Dt::I8: case Dt::P8: case Dt::B8: return 8;
        case Dt::S16: case Dt::U16: case Dt::I16: case Dt::P16: case Dt::B16: case Dt::F16: return 16;
        case Dt::S64: case Dt::U64: case Dt::I64: case Dt::B64: return 64;
        default: return 32;
    }
}
bool unsigned_dt(Dt dt) { return dt == Dt::U8 || dt == Dt::U16 || dt == Dt::U32 || dt == Dt::U64; }

// The register file seen as D registers and their elements (Elem[D[n], e, esize]).
struct Regs {
    PsvContext& ctx;
    u64 mask(u32 esize) const { return esize == 64 ? ~u64{0} : (u64{1} << esize) - 1; }
    u64 get(u32 d, u32 e, u32 esize) const {
        // (elements past the first D register continue into the next: Q registers, consecutive lists)
        const u32 per = 64 / esize;
        return (ctx.vfp.d[(d + e / per) & 31] >> ((e % per) * esize)) & mask(esize);
    }
    void set(u32 d, u32 e, u32 esize, u64 v) {
        const u32 per = 64 / esize, shift = (e % per) * esize;
        u64& r = ctx.vfp.d[(d + e / per) & 31];
        r = (r & ~(mask(esize) << shift)) | ((v & mask(esize)) << shift);
    }
};

i128 to_int(u64 v, u32 esize, bool is_unsigned) {
    if (is_unsigned || esize == 64) return is_unsigned ? static_cast<i128>(v) : static_cast<i128>(static_cast<i64>(v));
    const u64 sign = u64{1} << (esize - 1);
    return static_cast<i128>(static_cast<i64>((v ^ sign) - sign));
}

struct Sat {
    bool saturated = false;
    // SignedSatQ / UnsignedSatQ to esize bits.
    u64 operator()(i128 x, u32 esize, bool is_unsigned) {
        i128 hi, lo;
        if (is_unsigned) {
            hi = esize == 64 ? static_cast<i128>(~u64{0}) : static_cast<i128>((u64{1} << esize) - 1);
            lo = 0;
        } else {
            hi = static_cast<i128>((u64{1} << (esize - 1)) - 1);
            lo = -hi - 1;
        }
        if (x > hi) saturated = true, x = hi;
        if (x < lo) saturated = true, x = lo;
        return static_cast<u64>(x) & (esize == 64 ? ~u64{0} : (u64{1} << esize) - 1);
    }
};

// A vector operand: first D register, register count.
struct V {
    u32 d, regs;
};
V vec(const Operand& o) { return o.kind == OperandKind::QReg ? V{o.reg * 2u, 2} : V{o.reg, 1}; }
bool is_vec(const Operand& o) { return o.kind == OperandKind::QReg || o.kind == OperandKind::DReg; }

// Shift by a signed amount as VSHL (register) and its rounding/saturating variants define it.
i128 shift_by(i128 x, int shift, bool round) {
    if (shift >= 0) {
        if (shift >= 64) return x == 0 ? 0 : (x < 0 ? -(i128{1} << 100) : (i128{1} << 100));
        return x * (i128{1} << shift);
    }
    int r = -shift;
    if (r > 120) r = 120;
    return (x + (round ? (i128{1} << (r - 1)) : 0)) >> r;
}

u64 poly_mul(u64 a, u64 b, u32 bits) {
    u64 r = 0;
    for (u32 i = 0; i < bits; ++i)
        if ((b >> i) & 1) r ^= a << i;
    return r;
}

// ---- AdvSIMDExpandImm -------------------------------------------------------------------------------------------

u64 expand_imm(u32 op, u32 cmode, u32 imm8) {
    u64 imm32;
    switch (cmode >> 1) {
        case 0: imm32 = imm8; return imm32 | (imm32 << 32);
        case 1: imm32 = u64{imm8} << 8; return imm32 | (imm32 << 32);
        case 2: imm32 = u64{imm8} << 16; return imm32 | (imm32 << 32);
        case 3: imm32 = u64{imm8} << 24; return imm32 | (imm32 << 32);
        case 4: { const u64 h = imm8; return h | (h << 16) | (h << 32) | (h << 48); }
        case 5: { const u64 h = u64{imm8} << 8; return h | (h << 16) | (h << 32) | (h << 48); }
        case 6: imm32 = (cmode & 1) ? ((u64{imm8} << 16) | 0xFFFF) : ((u64{imm8} << 8) | 0xFF); return imm32 | (imm32 << 32);
        default: break;
    }
    if (!(cmode & 1)) {
        if (!op) {
            u64 r = 0;
            for (int i = 0; i < 8; ++i) r |= u64{imm8} << (8 * i);
            return r;
        }
        u64 r = 0;
        for (int i = 0; i < 8; ++i) r |= ((imm8 >> i) & 1 ? u64{0xFF} : u64{0}) << (8 * i);
        return r;
    }
    // imm8<7> : NOT(imm8<6>) : Replicate(imm8<6>, 5) : imm8<5:0> : Zeros(19)
    const u32 a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1;
    imm32 = (u64{a} << 31) | (u64{b ^ 1u} << 30) | (b ? u64{0x1F} << 25 : 0) | (u64{imm8 & 0x3F} << 19);
    return imm32 | (imm32 << 32);
}

class Simd {
public:
    explicit Simd(Exec& x) : x_(x), r_{x.ctx}, in_(x.in) {}

    bool run();

private:
    bool immediate();
    bool logic();
    bool same();
    bool longer();
    bool shift_imm();
    bool misc();
    bool other();
    bool memory();
    void finish(const Sat& s) {
        if (s.saturated) x_.ctx.fpscr |= 1u << 27;  // FPSCR.QC
    }

    Exec& x_;
    Regs r_;
    const Instruction& in_;
};

bool Simd::immediate() {
    if (in_.operand_count != 2 || in_.operands[1].kind != OperandKind::SimdImm) return false;
    const auto& s = in_.operands[1];
    const u64 imm = expand_imm(s.count, s.amount, s.index);
    const V d = vec(in_.operands[0]);
    for (u32 i = 0; i < d.regs; ++i) {
        u64& reg = x_.ctx.vfp.d[d.d + i];
        switch (in_.op) {
            case Op::Vmov: reg = imm; break;
            case Op::Vmvn: reg = ~imm; break;
            case Op::Vorr: reg |= imm; break;
            case Op::Vbic: reg &= ~imm; break;
            default: return false;
        }
    }
    return true;
}

bool Simd::logic() {
    switch (in_.op) {
        case Op::Vand: case Op::Vbic: case Op::Vorr: case Op::Vorn: case Op::Veor: case Op::Vbsl: case Op::Vbit: case Op::Vbif: break;
        default: return false;
    }
    if (in_.operand_count != 3 || !is_vec(in_.operands[2])) return false;
    const V d = vec(in_.operands[0]), n = vec(in_.operands[1]), m = vec(in_.operands[2]);
    for (u32 i = 0; i < d.regs; ++i) {
        const u64 op1 = x_.ctx.vfp.d[n.d + i], op2 = x_.ctx.vfp.d[m.d + i];
        u64& dst = x_.ctx.vfp.d[d.d + i];
        switch (in_.op) {
            case Op::Vand: dst = op1 & op2; break;
            case Op::Vbic: dst = op1 & ~op2; break;
            case Op::Vorr: dst = op1 | op2; break;
            case Op::Vorn: dst = op1 | ~op2; break;
            case Op::Veor: dst = op1 ^ op2; break;
            case Op::Vbsl: dst = (op1 & dst) | (op2 & ~dst); break;
            case Op::Vbit: dst = (op1 & op2) | (dst & ~op2); break;
            default: dst = (dst & op2) | (op1 & ~op2); break;  // vbif
        }
    }
    return true;
}

// Three registers of one element size, and the by-scalar forms with results of that size.
bool Simd::same() {
    if (in_.operand_count != 3 || !is_vec(in_.operands[0]) || !is_vec(in_.operands[1])) return false;
    const auto& third = in_.operands[2];
    const bool scalar = third.kind == OperandKind::DScalar;
    if (!scalar && !is_vec(third)) return false;
    const V d = vec(in_.operands[0]), n = vec(in_.operands[1]);
    const u32 m = scalar ? third.reg : vec(third).d;
    const u32 esize = esize_of(in_.dt), elements = d.regs * 64 / esize;
    const bool u = unsigned_dt(in_.dt);
    const auto op2 = [&](u32 e) { return scalar ? r_.get(m, third.index, esize) : r_.get(m, e, esize); };
    const bool pairwise = in_.op == Op::Vpadd || in_.op == Op::Vpmax || in_.op == Op::Vpmin;
    u64 out[16];
    if (in_.dt == Dt::F32) {
        const u32 std = fp::kStandard;
        const auto f = [&](u32 a, u32 b, u32 acc) -> u32 {
            switch (in_.op) {
                case Op::Vadd: case Op::Vpadd: return static_cast<u32>(fp::add(a, b, 32, std));
                case Op::Vsub: return static_cast<u32>(fp::sub(a, b, 32, std));
                case Op::Vmul: return static_cast<u32>(fp::mul(a, b, 32, std));
                case Op::Vabd: return static_cast<u32>(fp::abs(fp::sub(a, b, 32, std), 32));
                case Op::Vmla: return static_cast<u32>(fp::add(acc, fp::mul(a, b, 32, std), 32, std));
                case Op::Vmls: return static_cast<u32>(fp::sub(acc, fp::mul(a, b, 32, std), 32, std));
                case Op::Vceq: return fp::compare(a, b, 32, std) == 0b0110 ? ~0u : 0u;
                case Op::Vcge: { const u32 c = fp::compare(a, b, 32, std); return c == 0b0110 || c == 0b0010 ? ~0u : 0u; }
                case Op::Vcgt: return fp::compare(a, b, 32, std) == 0b0010 ? ~0u : 0u;
                case Op::Vacge: {
                    const u32 c = fp::compare(fp::abs(a, 32), fp::abs(b, 32), 32, std);
                    return c == 0b0110 || c == 0b0010 ? ~0u : 0u;
                }
                case Op::Vacgt: return fp::compare(fp::abs(a, 32), fp::abs(b, 32), 32, std) == 0b0010 ? ~0u : 0u;
                case Op::Vmax: case Op::Vpmax: return fp::max(a, b);
                case Op::Vmin: case Op::Vpmin: return fp::min(a, b);
                case Op::Vrecps: return fp::recip_step(a, b);
                default: return fp::rsqrt_step(a, b);  // vrsqrts
            }
        };
        switch (in_.op) {
            case Op::Vadd: case Op::Vsub: case Op::Vmul: case Op::Vabd: case Op::Vmla: case Op::Vmls: case Op::Vceq: case Op::Vcge:
            case Op::Vcgt: case Op::Vacge: case Op::Vacgt: case Op::Vmax: case Op::Vmin: case Op::Vrecps: case Op::Vrsqrts:
            case Op::Vpadd: case Op::Vpmax: case Op::Vpmin:
                break;
            default: return false;
        }
        if (pairwise) {
            out[0] = f(static_cast<u32>(r_.get(n.d, 0, 32)), static_cast<u32>(r_.get(n.d, 1, 32)), 0);
            out[1] = f(static_cast<u32>(r_.get(m, 0, 32)), static_cast<u32>(r_.get(m, 1, 32)), 0);
        } else {
            for (u32 e = 0; e < elements; ++e)
                out[e] = f(static_cast<u32>(r_.get(n.d, e, 32)), static_cast<u32>(op2(e)), static_cast<u32>(r_.get(d.d, e, 32)));
        }
        for (u32 e = 0; e < elements; ++e) r_.set(d.d, e, 32, out[e]);
        return true;
    }
    Sat sat;
    const u64 all = r_.mask(esize);
    const auto element = [&](u64 a, u64 b, u64 acc) -> u64 {
        const i128 x = to_int(a, esize, u), y = to_int(b, esize, u);
        switch (in_.op) {
            case Op::Vhadd: return static_cast<u64>((x + y) >> 1);
            case Op::Vrhadd: return static_cast<u64>((x + y + 1) >> 1);
            case Op::Vhsub: return static_cast<u64>((x - y) >> 1);
            case Op::Vqadd: return sat(x + y, esize, u);
            case Op::Vqsub: return sat(x - y, esize, u);
            case Op::Vcgt: return x > y ? all : 0;
            case Op::Vcge: return x >= y ? all : 0;
            case Op::Vshl: return static_cast<u64>(shift_by(x, static_cast<i8>(b & 0xFF), false));
            case Op::Vrshl: return static_cast<u64>(shift_by(x, static_cast<i8>(b & 0xFF), true));
            case Op::Vqshl: return sat(shift_by(x, static_cast<i8>(b & 0xFF), false), esize, u);
            case Op::Vqrshl: return sat(shift_by(x, static_cast<i8>(b & 0xFF), true), esize, u);
            case Op::Vmax: case Op::Vpmax: return static_cast<u64>(x >= y ? x : y);
            case Op::Vmin: case Op::Vpmin: return static_cast<u64>(x <= y ? x : y);
            case Op::Vabd: return static_cast<u64>(x >= y ? x - y : y - x);
            case Op::Vaba: return acc + static_cast<u64>(x >= y ? x - y : y - x);
            case Op::Vadd: case Op::Vpadd: return a + b;
            case Op::Vsub: return a - b;
            case Op::Vtst: return (a & b) != 0 ? all : 0;
            case Op::Vceq: return a == b ? all : 0;
            case Op::Vmla: return acc + a * b;
            case Op::Vmls: return acc - a * b;
            case Op::Vmul: return in_.dt == Dt::P8 ? poly_mul(a, b, 8) : a * b;
            case Op::Vqdmulh: return sat((2 * to_int(a, esize, false) * to_int(b, esize, false)) >> esize, esize, false);
            case Op::Vqrdmulh:
                return sat((2 * to_int(a, esize, false) * to_int(b, esize, false) + (i128{1} << (esize - 1))) >> esize, esize, false);
            default: return 0;
        }
    };
    switch (in_.op) {
        case Op::Vhadd: case Op::Vrhadd: case Op::Vhsub: case Op::Vqadd: case Op::Vqsub: case Op::Vcgt: case Op::Vcge: case Op::Vshl:
        case Op::Vrshl: case Op::Vqshl: case Op::Vqrshl: case Op::Vmax: case Op::Vmin: case Op::Vabd: case Op::Vaba: case Op::Vadd:
        case Op::Vsub: case Op::Vtst: case Op::Vceq: case Op::Vmla: case Op::Vmls: case Op::Vmul: case Op::Vqdmulh: case Op::Vqrdmulh:
        case Op::Vpadd: case Op::Vpmax: case Op::Vpmin:
            break;
        default: return false;
    }
    if (pairwise) {
        const u32 half = elements / 2;
        for (u32 e = 0; e < half; ++e) {
            out[e] = element(r_.get(n.d, 2 * e, esize), r_.get(n.d, 2 * e + 1, esize), 0);
            out[half + e] = element(r_.get(m, 2 * e, esize), r_.get(m, 2 * e + 1, esize), 0);
        }
    } else {
        for (u32 e = 0; e < elements; ++e) out[e] = element(r_.get(n.d, e, esize), op2(e), r_.get(d.d, e, esize));
    }
    for (u32 e = 0; e < elements; ++e) r_.set(d.d, e, esize, out[e]);
    finish(sat);
    return true;
}

// Long (Q = D op D), wide (Q = Q op D) and narrowing (D = high halves of Q op Q) operations, and the long by-scalar
// forms.
bool Simd::longer() {
    bool narrow = false;
    switch (in_.op) {
        case Op::Vaddl: case Op::Vaddw: case Op::Vsubl: case Op::Vsubw: case Op::Vabal: case Op::Vabdl: case Op::Vmlal: case Op::Vmlsl:
        case Op::Vmull: case Op::Vqdmlal: case Op::Vqdmlsl: case Op::Vqdmull:
            break;
        case Op::Vaddhn: case Op::Vraddhn: case Op::Vsubhn: case Op::Vrsubhn: narrow = true; break;
        default: return false;
    }
    const auto& third = in_.operands[2];
    const bool scalar = third.kind == OperandKind::DScalar;
    const u32 d = vec(in_.operands[0]).d, n = vec(in_.operands[1]).d, m = scalar ? third.reg : vec(third).d;
    if (narrow) {
        const u32 esize = esize_of(in_.dt) / 2, wide = 2 * esize, elements = 64 / esize;
        const bool round = in_.op == Op::Vraddhn || in_.op == Op::Vrsubhn;
        const bool add = in_.op == Op::Vaddhn || in_.op == Op::Vraddhn;
        u64 out[8];
        for (u32 e = 0; e < elements; ++e) {
            u64 result = add ? r_.get(n, e, wide) + r_.get(m, e, wide) : r_.get(n, e, wide) - r_.get(m, e, wide);
            if (round) result += u64{1} << (esize - 1);
            out[e] = (result & r_.mask(wide)) >> esize;
        }
        for (u32 e = 0; e < elements; ++e) r_.set(d, e, esize, out[e]);
        return true;
    }
    const u32 esize = esize_of(in_.dt), wide = 2 * esize, elements = 64 / esize;
    const bool u = unsigned_dt(in_.dt);
    const bool wide_n = in_.op == Op::Vaddw || in_.op == Op::Vsubw;
    Sat sat;
    u64 out[8];
    for (u32 e = 0; e < elements; ++e) {
        const i128 a = wide_n ? to_int(r_.get(n, e, wide), wide, u) : to_int(r_.get(n, e, esize), esize, u);
        const u64 b_raw = scalar ? r_.get(m, third.index, esize) : r_.get(m, e, esize);
        const i128 b = to_int(b_raw, esize, u), acc = to_int(r_.get(d, e, wide), wide, u);
        i128 result;
        switch (in_.op) {
            case Op::Vaddl: case Op::Vaddw: result = a + b; break;
            case Op::Vsubl: case Op::Vsubw: result = a - b; break;
            case Op::Vabal: result = acc + (a >= b ? a - b : b - a); break;
            case Op::Vabdl: result = a >= b ? a - b : b - a; break;
            case Op::Vmlal: result = acc + a * b; break;
            case Op::Vmlsl: result = acc - a * b; break;
            case Op::Vmull:
                result = in_.dt == Dt::P8 ? static_cast<i128>(poly_mul(r_.get(n, e, 8), b_raw, 8)) : a * b;
                break;
            default: {  // vqdmull, vqdmlal, vqdmlsl (signed)
                const u64 product = sat(2 * a * b, wide, false);
                if (in_.op == Op::Vqdmull) {
                    result = static_cast<i128>(product);
                } else {
                    const i128 p = to_int(product, wide, false);
                    result = static_cast<i128>(sat(in_.op == Op::Vqdmlal ? acc + p : acc - p, wide, false));
                }
                break;
            }
        }
        out[e] = static_cast<u64>(result);
    }
    for (u32 e = 0; e < elements; ++e) r_.set(d, e, wide, out[e]);
    finish(sat);
    return true;
}

bool Simd::shift_imm() {
    if (in_.op == Op::Vmovl) {
        const u32 d = vec(in_.operands[0]).d, m = vec(in_.operands[1]).d, esize = esize_of(in_.dt), elements = 64 / esize;
        const bool u = unsigned_dt(in_.dt);
        u64 out[8];
        for (u32 e = 0; e < elements; ++e) out[e] = static_cast<u64>(to_int(r_.get(m, e, esize), esize, u));
        for (u32 e = 0; e < elements; ++e) r_.set(d, e, 2 * esize, out[e]);
        return true;
    }
    if (in_.operand_count != 3 || in_.operands[2].kind != OperandKind::Imm || !is_vec(in_.operands[1])) return false;
    const V d = vec(in_.operands[0]), m = vec(in_.operands[1]);
    const u32 amount = in_.operands[2].imm;
    const bool u = unsigned_dt(in_.dt);
    Sat sat;
    switch (in_.op) {
        case Op::Vcvt: {
            const bool to_fixed = in_.dt == Dt::S32 || in_.dt == Dt::U32;
            const bool fixed_unsigned = (to_fixed ? in_.dt : in_.dt2) == Dt::U32;
            for (u32 e = 0; e < d.regs * 2; ++e) {
                const u64 v = r_.get(m.d, e, 32);
                const u64 r = to_fixed ? fp::to_fixed(v, 32, 32, static_cast<int>(amount), fixed_unsigned, true, fp::kStandard)
                                       : fp::from_fixed(v, 32, static_cast<int>(amount), fixed_unsigned, 32, true, fp::kStandard);
                r_.set(d.d, e, 32, r);
            }
            return true;
        }
        case Op::Vshrn: case Op::Vrshrn: case Op::Vqshrun: case Op::Vqrshrun: case Op::Vqshrn: case Op::Vqrshrn: {
            const u32 esize = esize_of(in_.dt) / 2, wide = 2 * esize, elements = 64 / esize;
            const bool round = in_.op == Op::Vrshrn || in_.op == Op::Vqrshrun || in_.op == Op::Vqrshrn;
            const bool to_unsigned = in_.op == Op::Vqshrun || in_.op == Op::Vqrshrun;
            u64 out[8];
            for (u32 e = 0; e < elements; ++e) {
                const i128 x = to_int(r_.get(m.d, e, wide), wide, to_unsigned ? false : u);
                const i128 s = shift_by(x, -static_cast<int>(amount), round);
                if (in_.op == Op::Vshrn || in_.op == Op::Vrshrn) out[e] = static_cast<u64>(s);
                else out[e] = sat(s, esize, to_unsigned ? true : u);
            }
            for (u32 e = 0; e < elements; ++e) r_.set(d.d, e, esize, out[e]);
            finish(sat);
            return true;
        }
        case Op::Vshll: {
            const u32 esize = esize_of(in_.dt), elements = 64 / esize;
            u64 out[8];
            for (u32 e = 0; e < elements; ++e) out[e] = static_cast<u64>(to_int(r_.get(m.d, e, esize), esize, u)) << amount;
            for (u32 e = 0; e < elements; ++e) r_.set(d.d, e, 2 * esize, out[e]);
            return true;
        }
        default: break;
    }
    const u32 esize = esize_of(in_.dt), elements = d.regs * 64 / esize;
    const u64 all = r_.mask(esize);
    u64 out[16];
    for (u32 e = 0; e < elements; ++e) {
        const u64 raw = r_.get(m.d, e, esize), dst = r_.get(d.d, e, esize);
        const i128 x = to_int(raw, esize, u);
        switch (in_.op) {
            case Op::Vshr: out[e] = static_cast<u64>(shift_by(x, -static_cast<int>(amount), false)); break;
            case Op::Vsra: out[e] = dst + static_cast<u64>(shift_by(x, -static_cast<int>(amount), false)); break;
            case Op::Vrshr: out[e] = static_cast<u64>(shift_by(x, -static_cast<int>(amount), true)); break;
            case Op::Vrsra: out[e] = dst + static_cast<u64>(shift_by(x, -static_cast<int>(amount), true)); break;
            case Op::Vsri: {
                // mask = LSR(Ones(esize), shift); Elem[Dd] = (Elem[Dd] AND NOT(mask)) OR LSR(Elem[Dm], shift)
                const u64 mask = amount >= esize ? 0 : all >> amount;
                out[e] = (dst & ~mask) | (amount >= esize ? 0 : raw >> amount);
                break;
            }
            case Op::Vshl: out[e] = raw << amount; break;
            case Op::Vsli: {
                const u64 mask = (all << amount) & all;
                out[e] = (dst & ~mask) | ((raw << amount) & mask);
                break;
            }
            case Op::Vqshlu: out[e] = sat(shift_by(to_int(raw, esize, false), static_cast<int>(amount), false), esize, true); break;
            case Op::Vqshl: out[e] = sat(shift_by(x, static_cast<int>(amount), false), esize, u); break;
            default: return false;
        }
    }
    for (u32 e = 0; e < elements; ++e) r_.set(d.d, e, esize, out[e]);
    finish(sat);
    return true;
}

// Integer reciprocal (square root) estimates (ARM ARM: UnsignedRecipEstimate, UnsignedRSqrtEstimate).
u32 unsigned_recip_estimate(u32 x) {
    if (!(x >> 31)) return 0xFFFFFFFFu;
    const u32 q = x >> 23;  // RoundDown(a * 512) for a = x * 2^-32 in [0.5, 1)
    const u32 s = (524288u + 2 * q + 1) / (4 * q + 2);  // RoundDown(256 / ((q + 0.5) / 512) + 0.5)
    return 0x80000000u | ((s - 256) << 23);
}
u32 rsqrt_s(u32 q, bool low_range) {
    // s = RoundDown(256 / sqrt(t) + 0.5), t = (q + 0.5) / 512 in [0.25, 0.5), or (q + 0.5) / 256 in [0.5, 1):
    // the largest s with (s - 0.5)^2 * t <= 65536.
    const u64 k = 2 * u64{q} + 1, limit = low_range ? 268435456ull : 134217728ull;
    u32 s = 512;
    while ((2 * u64{s} - 1) * (2 * u64{s} - 1) * k > limit) --s;
    return s;
}
u32 unsigned_rsqrt_estimate(u32 x) {
    if (!(x >> 30)) return 0xFFFFFFFFu;
    const u32 s = (x >> 31) ? rsqrt_s((x >> 24) & 0xFF, false) : rsqrt_s((x >> 23) & 0xFF, true);
    return 0x80000000u | ((s - 256) << 23);
}

bool Simd::misc() {
    const bool zero_form = in_.operand_count == 3 && in_.operands[2].kind == OperandKind::Imm &&
                           (in_.op == Op::Vcgt || in_.op == Op::Vcge || in_.op == Op::Vceq || in_.op == Op::Vcle || in_.op == Op::Vclt);
    if (in_.operand_count != 2 && !zero_form) return false;
    if (!is_vec(in_.operands[0]) || !is_vec(in_.operands[1])) return false;
    const V d = vec(in_.operands[0]), m = vec(in_.operands[1]);
    const bool f = in_.dt == Dt::F32, u = unsigned_dt(in_.dt);
    Sat sat;
    switch (in_.op) {
        case Op::Vswp:
            for (u32 i = 0; i < d.regs; ++i) std::swap(x_.ctx.vfp.d[d.d + i], x_.ctx.vfp.d[m.d + i]);
            return true;
        case Op::Vtrn: case Op::Vuzp: case Op::Vzip: {
            const u32 esize = esize_of(in_.dt), elements = d.regs * 64 / esize;
            u64 a[16], b[16], ra[16], rb[16];
            for (u32 e = 0; e < elements; ++e) a[e] = r_.get(d.d, e, esize), b[e] = r_.get(m.d, e, esize);
            for (u32 e = 0; e < elements; ++e) ra[e] = a[e], rb[e] = b[e];
            if (in_.op == Op::Vtrn) {
                for (u32 e = 0; e + 1 < elements; e += 2) ra[e + 1] = b[e], rb[e] = a[e + 1];
            } else if (in_.op == Op::Vuzp) {
                // The concatenation Dd:Dm (Dd first), unzipped: even elements to Dd, odd to Dm.
                u64 all[32];
                for (u32 e = 0; e < elements; ++e) all[e] = a[e], all[elements + e] = b[e];
                for (u32 e = 0; e < elements; ++e) ra[e] = all[2 * e], rb[e] = all[2 * e + 1];
            } else {
                u64 all[32];
                for (u32 e = 0; e < elements; ++e) all[2 * e] = a[e], all[2 * e + 1] = b[e];
                for (u32 e = 0; e < elements; ++e) ra[e] = all[e], rb[e] = all[elements + e];
            }
            for (u32 e = 0; e < elements; ++e) r_.set(d.d, e, esize, ra[e]), r_.set(m.d, e, esize, rb[e]);
            return true;
        }
        case Op::Vmovn: case Op::Vqmovun: case Op::Vqmovn: {
            const u32 esize = esize_of(in_.dt) / 2, elements = 64 / esize;
            u64 out[8];
            for (u32 e = 0; e < elements; ++e) {
                const u64 v = r_.get(m.d, e, 2 * esize);
                if (in_.op == Op::Vmovn) out[e] = v;
                else if (in_.op == Op::Vqmovun) out[e] = sat(to_int(v, 2 * esize, false), esize, true);
                else out[e] = sat(to_int(v, 2 * esize, u), esize, u);
            }
            for (u32 e = 0; e < elements; ++e) r_.set(d.d, e, esize, out[e]);
            finish(sat);
            return true;
        }
        case Op::Vcvt:
            if (in_.dt == Dt::F32 && in_.dt2 == Dt::F16) {  // Q d = D m (four halves)
                const u32 s = fp::kStandard | (x_.ctx.fpscr & fp::kAhp);
                u64 out[4];
                for (u32 e = 0; e < 4; ++e) out[e] = fp::convert(r_.get(m.d, e, 16), 16, 32, s);
                for (u32 e = 0; e < 4; ++e) r_.set(d.d, e, 32, out[e]);
                return true;
            }
            if (in_.dt == Dt::F16 && in_.dt2 == Dt::F32) {
                const u32 s = fp::kStandard | (x_.ctx.fpscr & fp::kAhp);
                u64 out[4];
                for (u32 e = 0; e < 4; ++e) out[e] = fp::convert(r_.get(m.d, e, 32), 32, 16, s);
                for (u32 e = 0; e < 4; ++e) r_.set(d.d, e, 16, out[e]);
                return true;
            }
            break;
        case Op::Vrev64: case Op::Vrev32: case Op::Vrev16: {
            const u32 esize = esize_of(in_.dt), group = (in_.op == Op::Vrev64 ? 64u : in_.op == Op::Vrev32 ? 32u : 16u) / esize;
            const u32 elements = d.regs * 64 / esize;
            u64 out[16];
            for (u32 e = 0; e < elements; ++e) out[e] = r_.get(m.d, e - e % group + (group - 1 - e % group), esize);
            for (u32 e = 0; e < elements; ++e) r_.set(d.d, e, esize, out[e]);
            return true;
        }
        case Op::Vpaddl: case Op::Vpadal: {
            const u32 esize = esize_of(in_.dt), elements = d.regs * 64 / esize;
            u64 out[8];
            for (u32 e = 0; e < elements / 2; ++e) {
                i128 sum = to_int(r_.get(m.d, 2 * e, esize), esize, u) + to_int(r_.get(m.d, 2 * e + 1, esize), esize, u);
                if (in_.op == Op::Vpadal) sum += to_int(r_.get(d.d, e, 2 * esize), 2 * esize, u);
                out[e] = static_cast<u64>(sum);
            }
            for (u32 e = 0; e < elements / 2; ++e) r_.set(d.d, e, 2 * esize, out[e]);
            return true;
        }
        default: break;
    }
    const u32 esize = in_.op == Op::Vmvn ? 8 : esize_of(in_.dt), elements = d.regs * 64 / esize;
    const u64 all = r_.mask(esize);
    const u32 std = fp::kStandard;
    u64 out[16];
    for (u32 e = 0; e < elements; ++e) {
        const u64 v = r_.get(m.d, e, esize);
        const i128 s = to_int(v, esize, false);
        const u32 fv = static_cast<u32>(v);
        u64 r;
        switch (in_.op) {
            case Op::Vcls: {
                r = 0;
                const u64 sign = (v >> (esize - 1)) & 1;
                for (int b = static_cast<int>(esize) - 2; b >= 0; --b) {
                    if (((v >> b) & 1) != sign) break;
                    ++r;
                }
                break;
            }
            case Op::Vclz: {
                r = 0;
                for (int b = static_cast<int>(esize) - 1; b >= 0 && !((v >> b) & 1); --b) ++r;
                break;
            }
            case Op::Vcnt: r = static_cast<u64>(std::popcount(v)); break;
            case Op::Vmvn: r = ~v; break;
            case Op::Vqabs: r = sat(s < 0 ? -s : s, esize, false); break;
            case Op::Vqneg: r = sat(-s, esize, false); break;
            case Op::Vabs: r = f ? fp::abs(fv, 32) : static_cast<u64>(s < 0 ? -s : s); break;
            case Op::Vneg: r = f ? fp::neg(fv, 32) : static_cast<u64>(-s); break;
            case Op::Vcgt: case Op::Vcge: case Op::Vceq: case Op::Vcle: case Op::Vclt: {
                bool t;
                if (f) {
                    const u32 c = fp::compare(fv, 0, 32, std);  // (NaN: 0011, all false)
                    const bool lt = c == 0b1000, eq = c == 0b0110, gt = c == 0b0010;
                    t = in_.op == Op::Vcgt ? gt : in_.op == Op::Vcge ? (gt || eq) : in_.op == Op::Vceq ? eq : in_.op == Op::Vcle ? (lt || eq) : lt;
                } else {
                    t = in_.op == Op::Vcgt ? s > 0 : in_.op == Op::Vcge ? s >= 0 : in_.op == Op::Vceq ? v == 0 : in_.op == Op::Vcle ? s <= 0 : s < 0;
                }
                r = t ? all : 0;
                break;
            }
            case Op::Vrecpe: r = f ? fp::recip_estimate(fv) : unsigned_recip_estimate(fv); break;
            case Op::Vrsqrte: r = f ? fp::rsqrt_estimate(fv) : unsigned_rsqrt_estimate(fv); break;
            case Op::Vcvt:
                if (in_.dt == Dt::S32 || in_.dt == Dt::U32) r = fp::to_fixed(fv, 32, 32, 0, in_.dt == Dt::U32, true, std);
                else r = fp::from_fixed(fv, 32, 0, in_.dt2 == Dt::U32, 32, true, std);
                break;
            default: return false;
        }
        out[e] = r;
    }
    for (u32 e = 0; e < elements; ++e) r_.set(d.d, e, esize, out[e]);
    finish(sat);
    return true;
}

bool Simd::other() {
    switch (in_.op) {
        case Op::Vext: {
            const V d = vec(in_.operands[0]), n = vec(in_.operands[1]), m = vec(in_.operands[2]);
            const u32 position = in_.operands[3].imm * esize_of(in_.dt) / 8, bytes = 8 * d.regs;
            u8 out[16];
            for (u32 i = 0; i < bytes; ++i) {
                const u32 at = position + i;  // in the concatenation Vm:Vn, Vn the low half
                out[i] = static_cast<u8>(at < bytes ? r_.get(n.d, at, 8) : r_.get(m.d, at - bytes, 8));
            }
            for (u32 i = 0; i < bytes; ++i) r_.set(d.d, i, 8, out[i]);
            return true;
        }
        case Op::Vtbl:
        case Op::Vtbx: {
            const u32 d = in_.operands[0].reg, n = in_.operands[1].reg, length = in_.operands[1].count, m = in_.operands[2].reg;
            u8 out[8];
            for (u32 i = 0; i < 8; ++i) {
                const u32 index = static_cast<u32>(r_.get(m, i, 8));
                if (index < 8 * length) out[i] = static_cast<u8>(r_.get(n, index, 8));
                else out[i] = in_.op == Op::Vtbx ? static_cast<u8>(r_.get(d, i, 8)) : 0;
            }
            for (u32 i = 0; i < 8; ++i) r_.set(d, i, 8, out[i]);
            return true;
        }
        case Op::Vdup: {
            const V d = vec(in_.operands[0]);
            const u32 esize = esize_of(in_.dt);
            const auto& src = in_.operands[1];
            const u64 value = src.kind == OperandKind::DScalar ? r_.get(src.reg, src.index, esize) : x_.reg(src.reg);
            for (u32 e = 0; e < d.regs * 64 / esize; ++e) r_.set(d.d, e, esize, value);
            return true;
        }
        default: return false;
    }
}

bool Simd::memory() {
    u32 n;
    bool load;
    switch (in_.op) {
        case Op::Vld1: n = 1, load = true; break;
        case Op::Vld2: n = 2, load = true; break;
        case Op::Vld3: n = 3, load = true; break;
        case Op::Vld4: n = 4, load = true; break;
        case Op::Vst1: n = 1, load = false; break;
        case Op::Vst2: n = 2, load = false; break;
        case Op::Vst3: n = 3, load = false; break;
        case Op::Vst4: n = 4, load = false; break;
        default: return false;
    }
    const auto& list = in_.operands[0];
    const auto& m = in_.operands[1];
    const u32 ebytes = esize_of(in_.dt) / 8;
    const u32 base = x_.reg(m.reg);
    // MemU[address, ebytes] (a 64-bit element is two word accesses, little-endian)
    const auto read = [&](u32 at) -> u64 {
        if (ebytes == 8) return x_.mem(at, 4) | (static_cast<u64>(x_.mem(at + 4, 4)) << 32);
        return x_.mem(at, ebytes);
    };
    const auto write = [&](u32 at, u64 v) {
        if (ebytes == 8) {
            x_.mem_write(at, 4, static_cast<u32>(v));
            x_.mem_write(at + 4, 4, static_cast<u32>(v >> 32));
        } else {
            x_.mem_write(at, ebytes, static_cast<u32>(v));
        }
    };
    u32 address = base, transferred;
    if (list.kind == OperandKind::DRegList) {
        // VLDn/VSTn (multiple n-element structures): for each register r, each element e, each structure member i.
        u32 regs, inc;
        if (n == 1) regs = list.count, inc = 0;
        else if (n == 2 && list.count == 4) regs = 2, inc = 2;
        else regs = 1, inc = list.index;
        for (u32 r = 0; r < regs; ++r)
            for (u32 e = 0; e < 8 / ebytes; ++e)
                for (u32 i = 0; i < n; ++i) {
                    const u32 reg = list.reg + i * inc + r;
                    if (load) r_.set(reg, e, 8 * ebytes, read(address));
                    else write(address, r_.get(reg, e, 8 * ebytes));
                    address += ebytes;
                }
        transferred = 8 * list.count;
    } else if (list.flags & arm::kAllLanes) {
        const u32 targets = n == 1 ? list.count : n;
        for (u32 i = 0; i < targets; ++i) {
            const u32 reg = list.reg + i * list.amount;
            const u64 v = read(base + (n == 1 ? 0 : i * ebytes));
            for (u32 e = 0; e < 8 / ebytes; ++e) r_.set(reg, e, 8 * ebytes, v);
        }
        transferred = n * ebytes;
    } else {
        for (u32 i = 0; i < n; ++i) {
            const u32 reg = list.reg + i * list.amount;
            if (load) r_.set(reg, list.index, 8 * ebytes, read(address));
            else write(address, r_.get(reg, list.index, 8 * ebytes));
            address += ebytes;
        }
        transferred = n * ebytes;
    }
    if (m.flags & arm::kMemPostReg) x_.ctx.r[m.reg] = base + x_.reg(m.reg2);
    else if (m.flags & arm::kMemWriteBack) x_.ctx.r[m.reg] = base + transferred;
    return true;
}

bool Simd::run() { return immediate() || logic() || memory() || other() || longer() || shift_imm() || same() || misc(); }

}  // namespace

bool exec_simd(Exec& x) { return Simd(x).run(); }

}  // namespace psvrecore::interp::detail
