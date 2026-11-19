// Advanced SIMD instructions on the registers' values (the D and Q registers in locals: analysis/vfp.hpp): each
// becomes a guest_neon.h helper on PsvD/PsvQ values, its operands read from and its results written to the locals
// that hold them. The same instructions as emit_simd.cpp's context form, with the same results; an instruction this
// file has no form for returns false and gets the context form (the locals synced around it).

#include "emit.hpp"

namespace psvrecore::recomp::detail {

namespace {

using arm::Dt;

u32 esize_of(Dt dt) {
    switch (dt) {
        case Dt::S8: case Dt::U8: case Dt::I8: case Dt::P8: case Dt::B8: return 8;
        case Dt::S16: case Dt::U16: case Dt::I16: case Dt::P16: case Dt::B16: case Dt::F16: return 16;
        case Dt::S64: case Dt::U64: case Dt::I64: case Dt::B64: return 64;
        default: return 32;
    }
}
bool is_signed(Dt dt) { return dt == Dt::S8 || dt == Dt::S16 || dt == Dt::S32 || dt == Dt::S64; }
// The element type tag of the helpers: s<e> or u<e> (by signedness where it matters, else u).
nx::string tag(Dt dt, bool signed_matters = true) { return nx::format("{}{}", signed_matters && is_signed(dt) ? "s" : "u", esize_of(dt)); }
nx::string utag(u32 esize) { return nx::format("u{}", esize); }

// A vector operand: its first D register and register count.
struct V {
    u32 d, regs;
    bool q() const { return regs == 2; }
    const char* w() const { return regs == 2 ? "q" : "d"; }
};
V vec(const Operand& o) { return o.kind == OperandKind::QReg ? V{o.reg * 2u, 2} : V{o.reg, 1}; }
bool is_vector(const Operand& o) { return o.kind == OperandKind::QReg || o.kind == OperandKind::DReg; }

u32 dmask(V v) { return (v.regs == 2 ? 3u : 1u) << v.d; }
// (no 32-bit lane of the constant is a denormal)
bool flushed_const(u64 imm) {
    for (const u32 w : {static_cast<u32>(imm), static_cast<u32>(imm >> 32)})
        if ((w & 0x7F800000u) == 0 && (w & 0x7FFFFFFFu) != 0) return false;
    return true;
}

nx::string get(Emitter& e, V v) { return v.q() ? e.vq(v.d / 2) : e.vdv(v.d); }
void put(Emitter& e, V v, nx::string_view x) {
    if (v.q()) e.set_vq(v.d / 2, x);
    else e.set_vdv(v.d, x);
}
// The by-scalar forms' operand: lane `index` of D register `reg` in every element of a register of size `w`.
nx::string dup_lane(Emitter& e, u32 reg, u32 index, u32 esize, const char* w) {
    return nx::format("psv_ndupl_{}_{}({}, {})", esize, w, e.vdv(reg), index);
}
nx::string dconst(u64 v) { return nx::format("psv_dvb(0x{:016X}ull)", v); }
nx::string vconst(V v, u64 imm) { return v.q() ? nx::format("psv_qconst(0x{:016X}ull, 0x{:016X}ull)", imm, imm) : dconst(imm); }

bool immediate(Emitter& e, const Instruction& in) {
    if (in.operand_count != 2 || in.operands[1].kind != OperandKind::SimdImm || !is_vector(in.operands[0])) return false;
    const auto& s = in.operands[1];
    const u64 imm = simd_expand_imm(s.count, s.amount, s.index);
    const V d = vec(in.operands[0]);
    switch (in.op) {
        case Op::Vmov:
            put(e, d, vconst(d, imm));
            if (flushed_const(imm)) e.produces_flushed(dmask(d));
            return true;
        case Op::Vmvn: put(e, d, vconst(d, ~imm)); return true;
        case Op::Vorr: put(e, d, nx::format("psv_norr_{}({}, {})", d.w(), get(e, d), vconst(d, imm))); return true;
        case Op::Vbic: put(e, d, nx::format("psv_nand_{}({}, {})", d.w(), get(e, d), vconst(d, ~imm))); return true;
        default: return false;
    }
}

bool logic(Emitter& e, const Instruction& in) {
    const char* op;
    switch (in.op) {
        case Op::Vand: op = "and"; break;
        case Op::Vbic: op = "bic"; break;
        case Op::Vorr: op = "orr"; break;
        case Op::Vorn: op = "orn"; break;
        case Op::Veor: op = "eor"; break;
        case Op::Vbsl: op = "bsl"; break;
        case Op::Vbit: op = "bit"; break;
        case Op::Vbif: op = "bif"; break;
        default: return false;
    }
    if (in.operand_count != 3 || !is_vector(in.operands[0]) || !is_vector(in.operands[1]) || !is_vector(in.operands[2])) return false;
    const V d = vec(in.operands[0]), n = vec(in.operands[1]), m = vec(in.operands[2]);
    if (in.op == Op::Vorr && n.d == m.d) {  // (vmov)
        put(e, d, get(e, n));
        if (e.known_flushed(dmask(n))) e.produces_flushed(dmask(d));
        return true;
    }
    if (in.op == Op::Vbsl || in.op == Op::Vbit || in.op == Op::Vbif) {
        const nx::string a = get(e, d), b = get(e, n), c = get(e, m);
        put(e, d, nx::format("psv_n{}_{}({}, {}, {})", op, d.w(), a, b, c));
        return true;
    }
    const nx::string a = get(e, n), b = get(e, m);
    put(e, d, nx::format("psv_n{}_{}({}, {})", op, d.w(), a, b));
    return true;
}

// Three registers of one element size, or two and a scalar.
bool same(Emitter& e, const Instruction& in) {
    if (in.operand_count != 3 || !is_vector(in.operands[0]) || !is_vector(in.operands[1])) return false;
    const auto& third = in.operands[2];
    const bool scalar = third.kind == OperandKind::DScalar;
    if (!scalar && !is_vector(third)) return false;
    const V d = vec(in.operands[0]), n = vec(in.operands[1]);
    const u32 esize = esize_of(in.dt);
    const auto second = [&] { return scalar ? dup_lane(e, third.reg, third.index, esize, d.w()) : get(e, vec(third)); };
    if (in.dt == Dt::F32) {
        const char* op = nullptr;
        bool acc = false, pairwise = false;
        switch (in.op) {
            case Op::Vadd: op = "add"; break;
            case Op::Vsub: op = "sub"; break;
            case Op::Vmul: op = "mul"; break;
            case Op::Vabd: op = "abd"; break;
            case Op::Vmla: op = "mla", acc = true; break;
            case Op::Vmls: op = "mls", acc = true; break;
            case Op::Vceq: op = "ceq"; break;
            case Op::Vcge: op = "cge"; break;
            case Op::Vcgt: op = "cgt"; break;
            case Op::Vacge: op = "acge"; break;
            case Op::Vacgt: op = "acgt"; break;
            case Op::Vmax: op = "max"; break;
            case Op::Vmin: op = "min"; break;
            case Op::Vrecps: op = "recps"; break;
            case Op::Vrsqrts: op = "rsqrts"; break;
            case Op::Vpadd: op = "padd", pairwise = true; break;
            case Op::Vpmax: op = "pmax", pairwise = true; break;
            case Op::Vpmin: op = "pmin", pairwise = true; break;
            default: return false;
        }
        if (pairwise && d.q()) return false;
        const nx::string fpscr = e.fpscr();
        // (operands known to hold no denormal are not flushed again: bit 0 n, bit 1 m, bit 2 d)
        u32 known = (e.known_flushed(dmask(n)) ? 1u : 0u) | (e.known_flushed(scalar ? 1u << third.reg : dmask(vec(third))) ? 2u : 0u) |
                    (acc && e.known_flushed(dmask(d)) ? 4u : 0u);
        const nx::string a = get(e, n);
        nx::string b;
        if (scalar && !(known & 2u)) {
            // (the scalar's D register flushed once, before it is duplicated: the same for each of its lanes' uses)
            b = nx::format("psv_ndupl_32_{}((PsvD)psv_nf_flush_d({}), {})", d.w(), e.vdv(third.reg), third.index);
            known |= 2u;
        } else {
            b = second();
        }
        if (acc) put(e, d, nx::format("psv_nf{}_{}({}, {}, {}, {}, {})", op, d.w(), fpscr, get(e, d), a, b, known));
        else put(e, d, nx::format("psv_nf{}_{}({}, {}, {}, {})", op, d.w(), fpscr, a, b, known));
        e.produces_flushed(dmask(d));
        return true;
    }
    // Integer elements. `sign`: the type's signedness matters; `qc`: saturating; `acc`: reads the destination.
    const char* op = nullptr;
    bool sign = true, qc = false, acc = false, pairwise = false;
    switch (in.op) {
        case Op::Vhadd: op = "hadd"; break;
        case Op::Vrhadd: op = "rhadd"; break;
        case Op::Vhsub: op = "hsub"; break;
        case Op::Vqadd: op = "qadd", qc = true; break;
        case Op::Vqsub: op = "qsub", qc = true; break;
        case Op::Vcgt: op = "cgt"; break;
        case Op::Vcge: op = "cge"; break;
        case Op::Vshl: op = "shl"; break;
        case Op::Vqshl: op = "qshl", qc = true; break;
        case Op::Vrshl: op = "rshl"; break;
        case Op::Vqrshl: op = "qrshl", qc = true; break;
        case Op::Vmax: op = "max"; break;
        case Op::Vmin: op = "min"; break;
        case Op::Vabd: op = "abd"; break;
        case Op::Vaba: op = "aba", acc = true; break;
        case Op::Vadd: op = "add", sign = false; break;
        case Op::Vsub: op = "sub", sign = false; break;
        case Op::Vtst: op = "tst", sign = false; break;
        case Op::Vceq: op = "ceq", sign = false; break;
        case Op::Vmla: op = "mla", sign = false, acc = true; break;
        case Op::Vmls: op = "mls", sign = false, acc = true; break;
        case Op::Vmul:
            if (in.dt == Dt::P8) {
                if (scalar) return false;
                const nx::string a = get(e, n), b = second();
                put(e, d, nx::format("psv_npmul_p8_{}({}, {})", d.w(), a, b));
                return true;
            }
            op = "mul", sign = false;
            break;
        case Op::Vqdmulh: op = "qdmulh", qc = true; break;
        case Op::Vqrdmulh: op = "qrdmulh", qc = true; break;
        case Op::Vpadd: op = "padd", sign = false, pairwise = true; break;
        case Op::Vpmax: op = "pmax", pairwise = true; break;
        case Op::Vpmin: op = "pmin", pairwise = true; break;
        default: return false;
    }
    if (pairwise && (d.q() || esize == 64)) return false;
    if ((in.op == Op::Vqdmulh || in.op == Op::Vqrdmulh) && esize != 16 && esize != 32) return false;
    if (scalar && esize == 8) return false;
    // (vshl and kin by a register are {d, m, n}: the value first, the shift amounts second)
    const nx::string t = tag(in.dt, sign);
    nx::string args;
    if (qc) args = e.fpscr_qc() + ", ";
    if (acc) args += get(e, d) + ", ";
    args += get(e, n) + ", ";
    args += second();
    put(e, d, nx::format("psv_n{}_{}_{}({})", op, t, d.w(), args));
    return true;
}

// Long, wide and narrow three-register operations (and the long by-scalar ones).
bool long_ops(Emitter& e, const Instruction& in) {
    const char* op = nullptr;
    bool narrow = false, wide = false, acc = false, qc = false, sign = true;
    int round = -1;
    switch (in.op) {
        case Op::Vaddl: op = "addl"; break;
        case Op::Vaddw: op = "addw", wide = true; break;
        case Op::Vsubl: op = "subl"; break;
        case Op::Vsubw: op = "subw", wide = true; break;
        case Op::Vabal: op = "abal", acc = true; break;
        case Op::Vabdl: op = "abdl"; break;
        case Op::Vmlal: op = "mlal", acc = true; break;
        case Op::Vmlsl: op = "mlsl", acc = true; break;
        case Op::Vmull: op = "mull"; break;
        case Op::Vqdmlal: op = "qdmlal", acc = true, qc = true; break;
        case Op::Vqdmlsl: op = "qdmlsl", acc = true, qc = true; break;
        case Op::Vqdmull: op = "qdmull", qc = true; break;
        case Op::Vaddhn: op = "addhn", narrow = true, round = 0; break;
        case Op::Vraddhn: op = "addhn", narrow = true, round = 1; break;
        case Op::Vsubhn: op = "subhn", narrow = true, round = 0; break;
        case Op::Vrsubhn: op = "subhn", narrow = true, round = 1; break;
        default: return false;
    }
    if (in.operand_count != 3 || !is_vector(in.operands[0]) || !is_vector(in.operands[1])) return false;
    const auto& third = in.operands[2];
    const bool scalar = third.kind == OperandKind::DScalar;
    if (!scalar && !is_vector(third)) return false;
    const V d = vec(in.operands[0]), n = vec(in.operands[1]);
    if (narrow) {  // (the wide element type: the helpers' unsigned one of that size)
        if (scalar) return false;
        const nx::string a = get(e, n), b = get(e, vec(third));
        put(e, d, nx::format("psv_n{}_{}({}, {}, {})", op, utag(esize_of(in.dt)), a, b, round));
        return true;
    }
    const u32 esize = esize_of(in.dt);
    if (esize == 64) return false;
    nx::string t = tag(in.dt, sign);
    if (in.op == Op::Vmull && in.dt == Dt::P8) {
        if (scalar) return false;
        t = "p8";
    }
    if (qc && esize != 16 && esize != 32) return false;
    if (qc && !is_signed(in.dt)) return false;
    const nx::string b = scalar ? dup_lane(e, third.reg, third.index, esize, "d") : get(e, vec(third));
    nx::string args;
    if (qc) args = e.fpscr_qc() + ", ";
    if (acc) args += get(e, d) + ", ";
    args += get(e, n) + ", " + b;
    (void)wide;
    put(e, d, nx::format("psv_n{}_{}({})", op, t, args));
    return true;
}

bool shifts(Emitter& e, const Instruction& in) {
    if (in.op == Op::Vmovl) {
        if (!is_vector(in.operands[0]) || !is_vector(in.operands[1])) return false;
        const V d = vec(in.operands[0]), m = vec(in.operands[1]);
        if (esize_of(in.dt) == 64) return false;
        put(e, d, nx::format("psv_nshll_{}({}, 0)", tag(in.dt), get(e, m)));
        return true;
    }
    if (in.operand_count != 3 || in.operands[2].kind != OperandKind::Imm || !is_vector(in.operands[0]) || !is_vector(in.operands[1]))
        return false;
    const V d = vec(in.operands[0]), m = vec(in.operands[1]);
    const u32 amount = in.operands[2].imm;
    const u32 esize = esize_of(in.dt);
    const nx::string w = d.w();
    switch (in.op) {
        case Op::Vshr:
        case Op::Vrshr: {
            if (amount < 1 || amount > esize) return false;
            put(e, d, nx::format("psv_n{}_{}_{}({}, {})", in.op == Op::Vshr ? "shr" : "rshr", tag(in.dt), w, get(e, m), amount));
            return true;
        }
        case Op::Vsra:
        case Op::Vrsra: {
            if (amount < 1 || amount > esize) return false;
            const nx::string acc = get(e, d), x = get(e, m);
            put(e, d, nx::format("psv_n{}_{}_{}({}, {}, {})", in.op == Op::Vsra ? "sra" : "rsra", tag(in.dt), w, acc, x, amount));
            return true;
        }
        case Op::Vsri:
        case Op::Vsli: {
            const bool right = in.op == Op::Vsri;
            if (right ? (amount < 1 || amount > esize) : amount >= esize) return false;
            const nx::string dst = get(e, d), x = get(e, m);
            put(e, d, nx::format("psv_n{}_{}_{}({}, {}, {})", right ? "sri" : "sli", esize, w, dst, x, amount));
            return true;
        }
        case Op::Vshl:
            if (amount >= esize) return false;
            put(e, d, nx::format("psv_nshli_{}_{}({}, {})", esize, w, get(e, m), amount));
            return true;
        case Op::Vqshl:
            if (amount >= esize) return false;
            put(e, d, nx::format("psv_nqshli_{}_{}({}, {}, {})", tag(in.dt), w, e.fpscr_qc(), get(e, m), amount));
            return true;
        case Op::Vqshlu:
            if (amount >= esize) return false;
            put(e, d, nx::format("psv_nqshlu_s{}_{}({}, {}, {})", esize, w, e.fpscr_qc(), get(e, m), amount));
            return true;
        case Op::Vshrn:
        case Op::Vrshrn:
        case Op::Vqshrn:
        case Op::Vqrshrn:
        case Op::Vqshrun:
        case Op::Vqrshrun: {  // (the wide element type named; amount 1 ... the narrow size)
            if (d.q() || !m.q() || esize == 8 || amount < 1 || amount > esize / 2) return false;
            const nx::string x = get(e, m);
            if (in.op == Op::Vshrn || in.op == Op::Vrshrn) {
                put(e, d, nx::format("psv_n{}_{}({}, {})", in.op == Op::Vshrn ? "shrn" : "rshrn", utag(esize), x, amount));
                return true;
            }
            const bool un = in.op == Op::Vqshrun || in.op == Op::Vqrshrun;
            const bool rnd = in.op == Op::Vqrshrn || in.op == Op::Vqrshrun;
            if (un && !is_signed(in.dt)) return false;
            const nx::string t = un ? nx::format("s{}", esize) : tag(in.dt);
            put(e, d, nx::format("psv_nq{}shr{}_{}({}, {}, {})", rnd ? "r" : "", un ? "un" : "n", t, e.fpscr_qc(), x, amount));
            return true;
        }
        case Op::Vshll: {
            if (!d.q() || m.q() || esize == 64 || amount > esize) return false;
            put(e, d, nx::format("psv_nshll_{}({}, {})", tag(in.dt), get(e, m), amount));
            return true;
        }
        case Op::Vcvt: {  // between single precision and fixed point
            if (amount < 1 || amount > 32) return false;
            const bool to_fixed = in.dt == Dt::S32 || in.dt == Dt::U32;
            const Dt fixed = to_fixed ? in.dt : in.dt2;
            if (fixed != Dt::S32 && fixed != Dt::U32) return false;
            const char* name = to_fixed ? (fixed == Dt::U32 ? "u32_f32" : "s32_f32") : (fixed == Dt::U32 ? "f32_u32" : "f32_s32");
            put(e, d, nx::format("psv_ncvtx_{}_{}({}, {})", name, w, get(e, m), amount));
            if (!to_fixed) e.produces_flushed(dmask(d));  // (a nonzero result is at least 2^-32)
            return true;
        }
        default: return false;
    }
}

bool misc(Emitter& e, const Instruction& in) {
    const bool zero_form = in.operand_count == 3 && in.operands[2].kind == OperandKind::Imm && in.operands[2].imm == 0 &&
                           (in.op == Op::Vcgt || in.op == Op::Vcge || in.op == Op::Vceq || in.op == Op::Vcle || in.op == Op::Vclt);
    if (in.operand_count != 2 && !zero_form) return false;
    if (!is_vector(in.operands[0]) || !is_vector(in.operands[1])) return false;
    const V d = vec(in.operands[0]), m = vec(in.operands[1]);
    const bool f = in.dt == Dt::F32;
    const u32 esize = esize_of(in.dt);
    const nx::string w = d.w();
    const auto unary = [&](nx::string_view name) { put(e, d, nx::format("psv_n{}_{}({})", name, w, get(e, m))); };
    switch (in.op) {
        case Op::Vrev64:
        case Op::Vrev32:
        case Op::Vrev16: {
            const u32 group = in.op == Op::Vrev64 ? 64 : in.op == Op::Vrev32 ? 32 : 16;
            if (esize >= group) return false;
            unary(nx::format("{}_{}", in.op == Op::Vrev64 ? "rev64" : in.op == Op::Vrev32 ? "rev32" : "rev16", esize));
            return true;
        }
        case Op::Vpaddl:
            if (esize == 64) return false;
            unary(nx::format("paddl_{}", tag(in.dt)));
            return true;
        case Op::Vpadal: {
            if (esize == 64) return false;
            const nx::string acc = get(e, d), x = get(e, m);
            put(e, d, nx::format("psv_npadal_{}_{}({}, {})", tag(in.dt), w, acc, x));
            return true;
        }
        case Op::Vcls: if (esize == 64) return false; unary(nx::format("cls_{}", esize)); return true;
        case Op::Vclz: if (esize == 64) return false; unary(nx::format("clz_{}", esize)); return true;
        case Op::Vcnt: if (esize != 8) return false; unary("cnt_8"); return true;
        case Op::Vmvn: unary("mvn"); return true;
        case Op::Vqabs:
        case Op::Vqneg:
            if (esize == 64) return false;
            put(e, d, nx::format("psv_nq{}_s{}_{}({}, {})", in.op == Op::Vqabs ? "abs" : "neg", esize, w, e.fpscr_qc(), get(e, m)));
            return true;
        case Op::Vcgt: case Op::Vcge: case Op::Vceq: case Op::Vcle: case Op::Vclt: {
            const char* c = in.op == Op::Vcgt ? "cgt0" : in.op == Op::Vcge ? "cge0" : in.op == Op::Vceq ? "ceq0" : in.op == Op::Vcle ? "cle0" : "clt0";
            if (f) {
                const nx::string fpscr = e.fpscr();
                put(e, d, nx::format("psv_nf{}_{}({}, {}, {})", c, w, fpscr, get(e, m), e.known_flushed(dmask(m)) ? 1 : 0));
                e.produces_flushed(dmask(d));
            } else {
                if (esize == 64) return false;
                put(e, d, nx::format("psv_n{}_s{}_{}({})", c, esize, w, get(e, m)));
            }
            return true;
        }
        case Op::Vabs:
        case Op::Vneg:
            if (f) {
                unary(in.op == Op::Vabs ? "fabs" : "fneg");
                if (e.known_flushed(dmask(m))) e.produces_flushed(dmask(d));
            }
            else if (esize == 64) return false;
            else unary(nx::format("{}_s{}", in.op == Op::Vabs ? "abs" : "neg", esize));
            return true;
        case Op::Vrecpe:
        case Op::Vrsqrte: {
            const char* name = in.op == Op::Vrecpe ? "recpe" : "rsqrte";
            if (f) {
                const nx::string fpscr = e.fpscr();
                put(e, d, nx::format("psv_nf{}_{}({}, {}, {})", name, w, fpscr, get(e, m), e.known_flushed(dmask(m)) ? 1 : 0));
                e.produces_flushed(dmask(d));
            } else if (in.dt == Dt::U32) {
                unary(nx::format("u{}", name));
            } else {
                return false;
            }
            return true;
        }
        case Op::Vmovn:
        case Op::Vqmovn:
        case Op::Vqmovun: {  // (the wide element type named)
            if (d.q() || !m.q() || esize == 8) return false;
            const nx::string x = get(e, m);
            if (in.op == Op::Vmovn) put(e, d, nx::format("psv_nmovn_{}({})", utag(esize), x));
            else if (in.op == Op::Vqmovn) put(e, d, nx::format("psv_nqmovn_{}({}, {})", tag(in.dt), e.fpscr_qc(), x));
            else put(e, d, nx::format("psv_nqmovun_s{}({}, {})", esize, e.fpscr_qc(), x));
            return true;
        }
        case Op::Vswp: {
            const nx::string a = e.temp("a"), b = e.temp("b");
            const char* type = d.q() ? "PsvQ" : "PsvD";
            e.line(nx::format("const {} {} = {}, {} = {};", type, a, get(e, d), b, get(e, m)));
            put(e, d, b);
            put(e, m, a);
            return true;
        }
        case Op::Vtrn:
        case Op::Vuzp:
        case Op::Vzip: {
            if (esize == 64) return false;
            const char* name = in.op == Op::Vtrn ? "trn" : in.op == Op::Vuzp ? "uzp" : "zip";
            // (vuzp.32 and vzip.32 of D registers are vtrn.32)
            if (!d.q() && esize == 32) name = "trn";
            const char* type = d.q() ? "PsvQ" : "PsvD";
            const nx::string first = e.temp("p"), second = e.temp("p");
            e.line(nx::format("{} {}; const {} {} = psv_n{}_{}_{}({}, {}, &{});", type, second, type, first, name, esize, w, get(e, d), get(e, m),
                              second));
            put(e, d, first);
            put(e, m, second);
            return true;
        }
        case Op::Vcvt:
            if (in.dt == Dt::F32 && in.dt2 == Dt::F16) {
                if (!d.q() || m.q()) return false;
                const nx::string fpscr = e.fpscr();
                put(e, d, nx::format("psv_ncvt_f32_f16({}, {})", fpscr, get(e, m)));
                return true;
            }
            if (in.dt == Dt::F16 && in.dt2 == Dt::F32) {
                if (d.q() || !m.q()) return false;
                const nx::string fpscr = e.fpscr();
                put(e, d, nx::format("psv_ncvt_f16_f32({}, {})", fpscr, get(e, m)));
                return true;
            }
            if (in.dt == Dt::S32 && in.dt2 == Dt::F32) unary("cvt_s32_f32");
            else if (in.dt == Dt::U32 && in.dt2 == Dt::F32) unary("cvt_u32_f32");
            else if (in.dt == Dt::F32 && in.dt2 == Dt::S32) unary("cvt_f32_s32"), e.produces_flushed(dmask(d));
            else if (in.dt == Dt::F32 && in.dt2 == Dt::U32) unary("cvt_f32_u32"), e.produces_flushed(dmask(d));
            else return false;
            return true;
        default: return false;
    }
}

bool other(Emitter& e, const Instruction& in) {
    switch (in.op) {
        case Op::Vext: {
            if (in.operand_count != 4 || !is_vector(in.operands[0])) return false;
            const V d = vec(in.operands[0]), n = vec(in.operands[1]), m = vec(in.operands[2]);
            const u32 start = in.operands[3].imm * esize_of(in.dt) / 8;
            if (start >= 8 * d.regs) return false;
            const nx::string a = get(e, n), b = get(e, m);
            put(e, d, nx::format("psv_next_{}({}, {}, {})", d.w(), a, b, start));
            return true;
        }
        case Op::Vtbl:
        case Op::Vtbx: {
            const auto& list = in.operands[1];
            const u32 len = list.count;
            if (len < 1 || len > 4) return false;
            const u32 d = in.operands[0].reg, n = list.reg, m = in.operands[2].reg;
            nx::string t[4];
            for (u32 i = 0; i < 4; ++i) t[i] = i < len ? e.vdv((n + i) & 31) : nx::string("psv_dvb(0)");
            const nx::string idx = e.vdv(m), old = in.op == Op::Vtbx ? e.vdv(d) : nx::string("psv_dvb(0)");
            e.set_vdv(d, nx::format("psv_ntblv(psv_qcat({}, {}), psv_qcat({}, {}), {}, {}, {}, {})", t[0], t[1], t[2], t[3], len, idx, old,
                                    in.op == Op::Vtbx ? 1 : 0));
            return true;
        }
        case Op::Vdup: {
            if (!is_vector(in.operands[0])) return false;
            const V d = vec(in.operands[0]);
            const u32 esize = esize_of(in.dt);
            if (esize == 64) return false;
            const auto& src = in.operands[1];
            if (src.kind == OperandKind::DScalar) {
                put(e, d, dup_lane(e, src.reg, src.index, esize, d.w()));
                if (esize == 32 && e.known_flushed(1u << src.reg)) e.produces_flushed(dmask(d));
            } else if (src.kind == OperandKind::Reg) {
                put(e, d, nx::format("psv_ndup_{}_{}({})", esize, d.w(), e.read(in, src.reg)));
            } else {
                return false;
            }
            return true;
        }
        default: return false;
    }
}

// VLD1-4 / VST1-4: the elements in the pseudocode's order (as emit_simd.cpp's memory()), each register's bytes read
// or written by whole-register (or two-register) host accesses, structures (de)interleaved by byte shuffles.
bool memory(Emitter& e, const Instruction& in) {
    u32 n;
    bool load;
    switch (in.op) {
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
    const auto& list = in.operands[0];
    const auto& m = in.operands[1];
    const u32 ebytes = esize_of(in.dt) / 8, esize = 8 * ebytes;
    const nx::string base = e.temp("base");
    e.line(nx::format("const uint32_t {} = {};", base, e.read(in, m.reg)));
    u32 size;
    if (list.kind == OperandKind::DRegList) {
        const u32 count = list.count;
        if (count < 1 || count > 4 || list.reg + count > 32) return false;
        size = 8 * count;
        // Memory element j (of `ebytes`) is lane `lane[j]` of register `reg[j]` (as list offsets 0-3).
        u32 regs, inc;
        if (n == 1) regs = count, inc = 1;
        else if (n == 2 && count == 4) regs = 2, inc = 2;
        else regs = 1, inc = list.index;
        const u32 elements = 8 / ebytes;
        nx::vector<u32> reg_of, lane_of;
        for (u32 r = 0; r < regs; ++r)
            for (u32 el = 0; el < elements; ++el)
                for (u32 i = 0; i < n; ++i) reg_of.push_back(i * inc + r), lane_of.push_back(el);
        if (reg_of.size() * ebytes != size) return false;
        for (u32 x : reg_of)
            if (x >= count && !(inc == 2)) return false;
        // The registers the list names, in order (offsets from list.reg): 0, 1, ... or with spacing 2.
        nx::vector<u32> named;
        for (u32 x : reg_of)
            if (std::find(named.begin(), named.end(), x) == named.end()) named.push_back(x);
        std::sort(named.begin(), named.end());
        if (named.size() != count) return false;
        for (u32 x : named)
            if (list.reg + x >= 32) return false;
        // Byte b of memory comes from byte k of the named registers' concatenation (k = 8 * position + byte).
        nx::vector<u32> src(size);
        for (u32 j = 0; j < reg_of.size(); ++j) {
            const u32 pos = static_cast<u32>(std::find(named.begin(), named.end(), reg_of[j]) - named.begin());
            for (u32 b = 0; b < ebytes; ++b) src[j * ebytes + b] = 8 * pos + lane_of[j] * ebytes + b;
        }
        const bool plain = n == 1;  // (VLD1/VST1: register bytes in order, src[b] == b)
        if (load) {
            // Chunks of 16 bytes (and 8 for the rest), then each register's bytes picked from them.
            nx::vector<nx::string> chunks;
            for (u32 off = 0; off < size; off += 16) {
                const nx::string c = e.temp("c");
                if (size - off >= 16) e.line(nx::format("const PsvQ {} = psv_mldq(mb, {} + {}u, {}u);", c, base, off, ebytes));
                else e.line(nx::format("const PsvQ {0} = psv_qcat(psv_mldd(mb, {1} + {2}u, {3}u), psv_dvb(0));", c, base, off, ebytes));
                chunks.push_back(c);
            }
            nx::vector<nx::string> value(count);
            for (u32 pos = 0; pos < count; ++pos) {
                // (the inverse map: register byte -> memory byte)
                u32 from[8];
                for (u32 b = 0; b < size; ++b)
                    if (src[b] / 8 == pos) from[src[b] % 8] = b;
                bool contiguous = true;
                for (u32 b = 0; b < 8; ++b) contiguous &= from[b] == from[0] + b && (from[0] % 8) == 0;
                if (contiguous) {
                    const nx::string& c = chunks[from[0] / 16];
                    value[pos] = nx::format("psv_q{}({})", (from[0] % 16) ? "hi" : "lo", c);
                } else {
                    // (bytes of up to 32: the two first chunks as one shuffle's two sources)
                    const nx::string& a = chunks[0];
                    const nx::string& b2 = chunks.size() > 1 ? chunks[1] : chunks[0];
                    nx::string idx;
                    for (u32 b = 0; b < 8; ++b) idx += nx::format("{}{}", b ? ", " : "", from[b]);
                    value[pos] = nx::format("(PsvD)__builtin_shufflevector((psv_u8_q){}, (psv_u8_q){}, {})", a, b2, idx);
                }
            }
            // Writes: a Q register whose two halves are both loaded at once (its whole value), else D by D.
            for (u32 pos = 0; pos < count; ++pos) {
                const u32 r = list.reg + named[pos];
                if ((r & 1) == 0 && pos + 1 < count && named[pos + 1] == named[pos] + 1) {
                    if (plain && (pos * 8) % 16 == 0) e.set_vq(r / 2, chunks[pos * 8 / 16]);
                    else e.set_vq(r / 2, nx::format("psv_qcat({}, {})", value[pos], value[pos + 1]));
                    ++pos;
                } else {
                    e.set_vdv(r, value[pos]);
                }
            }
        } else {
            // The named registers' values (a Q register's two halves together), then each 16 (or 8) bytes of memory.
            nx::vector<nx::string> q;  // PsvQ of positions 2k, 2k+1
            for (u32 pos = 0; pos < count; pos += 2) {
                const u32 r = list.reg + named[pos];
                const nx::string t = e.temp("sv");
                if (pos + 1 < count && named[pos + 1] == named[pos] + 1 && (r & 1) == 0) e.line(nx::format("const PsvQ {} = {};", t, e.vq(r / 2)));
                else if (pos + 1 < count) e.line(nx::format("const PsvQ {} = psv_qcat({}, {});", t, e.vdv(r), e.vdv(list.reg + named[pos + 1])));
                else e.line(nx::format("const PsvQ {} = psv_qcat({}, psv_dvb(0));", t, e.vdv(r)));
                q.push_back(t);
            }
            for (u32 off = 0; off < size; off += 16) {
                const u32 len = std::min<u32>(16, size - off);
                nx::string value;
                bool identity = true;
                for (u32 b = 0; b < len; ++b) identity &= src[off + b] == off + b;
                if (identity && len == 16) {
                    value = q[off / 16];
                } else if (identity) {
                    value = nx::format("psv_qlo({})", q[off / 16]);
                } else {
                    nx::string idx;
                    for (u32 b = 0; b < len; ++b) idx += nx::format("{}{}", b ? ", " : "", src[off + b]);
                    const nx::string& a = q[0];
                    const nx::string& b2 = q.size() > 1 ? q[1] : q[0];
                    value = nx::format("({})__builtin_shufflevector((psv_u8_q){}, (psv_u8_q){}, {})", len == 16 ? "PsvQ" : "PsvD", a, b2, idx);
                }
                if (len == 16) e.line(nx::format("psv_mstq(mb, {} + {}u, {}, {}u);", base, off, value, ebytes));
                else e.line(nx::format("psv_mstd(mb, {} + {}u, {}, {}u);", base, off, value, ebytes));
            }
        }
    } else if (list.kind == OperandKind::DLaneList && (list.flags & arm::kAllLanes)) {  // to all lanes (loads only)
        if (!load || esize == 64) return false;
        const u32 targets = n == 1 ? list.count : n;
        const char* ld = ebytes == 1 ? "psv_mld8" : ebytes == 2 ? "psv_mld16" : "psv_mld32";
        // (each element once, in order, then the registers)
        nx::vector<nx::string> v;
        for (u32 i = 0; i < n; ++i) {
            const nx::string t = e.temp("v");
            e.line(nx::format("const uint32_t {} = {}(mb, {} + {}u);", t, ld, base, i * ebytes));
            v.push_back(t);
        }
        for (u32 i = 0; i < targets; ++i) {
            const u32 reg = list.reg + i * list.amount;
            if (reg >= 32) return false;
            e.set_vdv(reg, nx::format("psv_ndup_{}_d({})", esize, v[n == 1 ? 0 : i]));
        }
        size = n * ebytes;
    } else if (list.kind == OperandKind::DLaneList) {  // one lane
        nx::vector<nx::string> v;
        for (u32 i = 0; i < n; ++i) {
            const u32 reg = list.reg + i * list.amount;
            if (reg >= 32) return false;
            const u32 at = i * ebytes;
            if (load) {
                const char* ld = ebytes == 1 ? "psv_mld8" : ebytes == 2 ? "psv_mld16" : ebytes == 4 ? "psv_mld32" : "psv_mld64";
                e.set_vdv(reg, nx::format("psv_nset_{}({}, {}, {}(mb, {} + {}u))", esize, e.vdv(reg), list.index, ld, base, at));
            } else {
                const char* st = ebytes == 1 ? "psv_mst8" : ebytes == 2 ? "psv_mst16" : ebytes == 4 ? "psv_mst32" : "psv_mst64";
                e.line(nx::format("{}(mb, {} + {}u, psv_nget_{}({}, {}));", st, base, at, esize, e.vdv(reg), list.index));
            }
        }
        size = n * ebytes;
    } else {
        return false;
    }
    if (m.flags & arm::kMemPostReg) {
        const nx::string step = e.read(in, m.reg2);
        e.line(e.assignment(m.reg, nx::format("{} + {}", base, step)));
    } else if (m.flags & arm::kMemWriteBack) {
        e.line(e.assignment(m.reg, nx::format("{} + {}u", base, size)));
    }
    return true;
}

}  // namespace

bool emit_neon(Emitter& e, const Instruction& in) {
    return immediate(e, in) || logic(e, in) || memory(e, in) || other(e, in) || long_ops(e, in) || shifts(e, in) || same(e, in) ||
           misc(e, in);
}

}  // namespace psvrecore::recomp::detail
