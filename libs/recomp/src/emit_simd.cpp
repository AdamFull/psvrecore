// Advanced SIMD instructions: each becomes a call of guest_simd.h's helper for its group, with the operation,
// element size and registers as constants. Element and structure loads and stores are unrolled here.

#include <bit>

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
bool is_unsigned(Dt dt) { return dt == Dt::U8 || dt == Dt::U16 || dt == Dt::U32 || dt == Dt::U64; }

// A vector operand: its first D register and register count.
struct V {
    u32 d, regs;
};
V vec(const Operand& o) { return o.kind == OperandKind::QReg ? V{o.reg * 2u, 2} : V{o.reg, 1}; }
bool is_vector(const Operand& o) { return o.kind == OperandKind::QReg || o.kind == OperandKind::DReg; }

// AdvSIMDExpandImm.
u64 expand_imm(u32 op, u32 cmode, u32 imm8) {
    const auto rep2 = [](u64 v) { return v | (v << 32); };
    const auto rep4 = [](u64 v) { return v | (v << 16) | (v << 32) | (v << 48); };
    switch (cmode >> 1) {
        case 0: return rep2(imm8);
        case 1: return rep2(u64{imm8} << 8);
        case 2: return rep2(u64{imm8} << 16);
        case 3: return rep2(u64{imm8} << 24);
        case 4: return rep4(imm8);
        case 5: return rep4(u64{imm8} << 8);
        case 6: return (cmode & 1) ? rep2((u64{imm8} << 16) | 0xFFFF) : rep2((u64{imm8} << 8) | 0xFF);
        default:
            if (!(cmode & 1) && !op) return imm8 * 0x0101010101010101ull;
            if (!(cmode & 1) && op) {
                u64 v = 0;
                for (int i = 0; i < 8; ++i)
                    if ((imm8 >> i) & 1) v |= u64{0xFF} << (8 * i);
                return v;
            }
            {  // a single-precision value
                const u32 b7 = (imm8 >> 7) & 1, b6 = (imm8 >> 6) & 1;
                const u32 f = (b7 << 31) | ((b6 ^ 1) << 30) | (b6 ? 0x3E000000u : 0u) | ((imm8 & 0x3F) << 19);
                return rep2(f);
            }
    }
}

bool immediate(Emitter& e, const Instruction& in) {
    if (in.operand_count != 2 || in.operands[1].kind != OperandKind::SimdImm) return false;
    const auto& s = in.operands[1];
    const u64 imm = expand_imm(s.count, s.amount, s.index);
    const V d = vec(in.operands[0]);
    for (u32 i = 0; i < d.regs; ++i) {
        const nx::string r = nx::format("ctx->vfp.d[{}]", d.d + i);
        switch (in.op) {
            case Op::Vmov: e.line(nx::format("{} = 0x{:016X}ull;", r, imm)); break;
            case Op::Vmvn: e.line(nx::format("{} = 0x{:016X}ull;", r, ~imm)); break;
            case Op::Vorr: e.line(nx::format("{} |= 0x{:016X}ull;", r, imm)); break;
            case Op::Vbic: e.line(nx::format("{} &= 0x{:016X}ull;", r, ~imm)); break;
            default: return false;
        }
    }
    return true;
}

bool logic(Emitter& e, const Instruction& in) {
    const char* op;
    switch (in.op) {
        case Op::Vand: op = "PSV_N_AND"; break;
        case Op::Vbic: op = "PSV_N_BIC"; break;
        case Op::Vorr: op = "PSV_N_ORR"; break;
        case Op::Vorn: op = "PSV_N_ORN"; break;
        case Op::Veor: op = "PSV_N_EOR"; break;
        case Op::Vbsl: op = "PSV_N_BSL"; break;
        case Op::Vbit: op = "PSV_N_BIT"; break;
        case Op::Vbif: op = "PSV_N_BIF"; break;
        default: return false;
    }
    if (in.operand_count != 3 || !is_vector(in.operands[2])) return false;
    const V d = vec(in.operands[0]), n = vec(in.operands[1]), m = vec(in.operands[2]);
    e.line(nx::format("psv_nlogic(ctx, {}, {}, {}, {}, {});", op, d.regs, d.d, n.d, m.d));
    return true;
}

// Integer and floating-point operations on elements of one size (three registers, or two and a scalar).
bool same(Emitter& e, const Instruction& in) {
    if (in.operand_count != 3 || !is_vector(in.operands[0]) || !is_vector(in.operands[1])) return false;
    const auto& third = in.operands[2];
    const bool scalar = third.kind == OperandKind::DScalar;
    if (!scalar && !is_vector(third)) return false;
    // vshl and kin (register forms) are {d, m, n}: the value first, the shift amounts second.
    const V d = vec(in.operands[0]), n = vec(in.operands[1]);
    const u32 m = scalar ? third.reg : vec(third).d;
    const int index = scalar ? third.index : -1;
    if (in.dt == Dt::F32) {
        const char* op = nullptr;
        switch (in.op) {
            case Op::Vadd: op = "PSV_NF_ADD"; break;
            case Op::Vsub: op = "PSV_NF_SUB"; break;
            case Op::Vmul: op = "PSV_NF_MUL"; break;
            case Op::Vabd: op = "PSV_NF_ABD"; break;
            case Op::Vmla: op = "PSV_NF_MLA"; break;
            case Op::Vmls: op = "PSV_NF_MLS"; break;
            case Op::Vceq: op = "PSV_NF_CEQ"; break;
            case Op::Vcge: op = "PSV_NF_CGE"; break;
            case Op::Vcgt: op = "PSV_NF_CGT"; break;
            case Op::Vacge: op = "PSV_NF_ACGE"; break;
            case Op::Vacgt: op = "PSV_NF_ACGT"; break;
            case Op::Vmax: op = "PSV_NF_MAX"; break;
            case Op::Vmin: op = "PSV_NF_MIN"; break;
            case Op::Vrecps: op = "PSV_NF_RECPS"; break;
            case Op::Vrsqrts: op = "PSV_NF_RSQRTS"; break;
            case Op::Vpadd: op = "PSV_NF_PADD"; break;
            case Op::Vpmax: op = "PSV_NF_PMAX"; break;
            case Op::Vpmin: op = "PSV_NF_PMIN"; break;
            default: return false;
        }
        e.line(nx::format("psv_nf3(ctx, {}, {}, {}, {}, {}, {});", op, d.regs, d.d, n.d, m, index));
        return true;
    }
    const char* op = nullptr;
    switch (in.op) {
        case Op::Vhadd: op = "PSV_N_HADD"; break;
        case Op::Vrhadd: op = "PSV_N_RHADD"; break;
        case Op::Vhsub: op = "PSV_N_HSUB"; break;
        case Op::Vqadd: op = "PSV_N_QADD"; break;
        case Op::Vqsub: op = "PSV_N_QSUB"; break;
        case Op::Vcgt: op = "PSV_N_CGT"; break;
        case Op::Vcge: op = "PSV_N_CGE"; break;
        case Op::Vshl: op = "PSV_N_SHL"; break;
        case Op::Vqshl: op = "PSV_N_QSHL"; break;
        case Op::Vrshl: op = "PSV_N_RSHL"; break;
        case Op::Vqrshl: op = "PSV_N_QRSHL"; break;
        case Op::Vmax: op = "PSV_N_MAX"; break;
        case Op::Vmin: op = "PSV_N_MIN"; break;
        case Op::Vabd: op = "PSV_N_ABD"; break;
        case Op::Vaba: op = "PSV_N_ABA"; break;
        case Op::Vadd: op = "PSV_N_ADD"; break;
        case Op::Vsub: op = "PSV_N_SUB"; break;
        case Op::Vtst: op = "PSV_N_TST"; break;
        case Op::Vceq: op = "PSV_N_CEQ"; break;
        case Op::Vmla: op = "PSV_N_MLA"; break;
        case Op::Vmls: op = "PSV_N_MLS"; break;
        case Op::Vmul: op = in.dt == Dt::P8 ? "PSV_N_MULP" : "PSV_N_MUL"; break;
        case Op::Vqdmulh: op = "PSV_N_QDMULH"; break;
        case Op::Vqrdmulh: op = "PSV_N_QRDMULH"; break;
        case Op::Vpadd: op = "PSV_N_PADD"; break;
        case Op::Vpmax: op = "PSV_N_PMAX"; break;
        case Op::Vpmin: op = "PSV_N_PMIN"; break;
        default: return false;
    }
    e.line(nx::format("psv_n3(ctx, {}, {}, {}, {}, {}, {}, {}, {});", op, esize_of(in.dt), is_unsigned(in.dt) ? 1 : 0, d.regs, d.d, n.d,
                      m, index));
    return true;
}

// Long, wide and narrow three-register operations (and the long by-scalar ones).
bool long_ops(Emitter& e, const Instruction& in) {
    const char* op = nullptr;
    bool narrow = false;
    switch (in.op) {
        case Op::Vaddl: op = "PSV_NL_ADDL"; break;
        case Op::Vaddw: op = "PSV_NL_ADDW"; break;
        case Op::Vsubl: op = "PSV_NL_SUBL"; break;
        case Op::Vsubw: op = "PSV_NL_SUBW"; break;
        case Op::Vabal: op = "PSV_NL_ABAL"; break;
        case Op::Vabdl: op = "PSV_NL_ABDL"; break;
        case Op::Vmlal: op = "PSV_NL_MLAL"; break;
        case Op::Vmlsl: op = "PSV_NL_MLSL"; break;
        case Op::Vmull: op = in.dt == Dt::P8 ? "PSV_NL_MULLP" : "PSV_NL_MULL"; break;
        case Op::Vqdmlal: op = "PSV_NL_QDMLAL"; break;
        case Op::Vqdmlsl: op = "PSV_NL_QDMLSL"; break;
        case Op::Vqdmull: op = "PSV_NL_QDMULL"; break;
        case Op::Vaddhn: op = "PSV_NL_ADDHN", narrow = true; break;
        case Op::Vraddhn: op = "PSV_NL_RADDHN", narrow = true; break;
        case Op::Vsubhn: op = "PSV_NL_SUBHN", narrow = true; break;
        case Op::Vrsubhn: op = "PSV_NL_RSUBHN", narrow = true; break;
        default: return false;
    }
    const auto& third = in.operands[2];
    const bool scalar = third.kind == OperandKind::DScalar;
    const u32 esize = narrow ? esize_of(in.dt) / 2 : esize_of(in.dt);
    const V d = vec(in.operands[0]), n = vec(in.operands[1]);
    const u32 m = scalar ? third.reg : vec(third).d;
    e.line(nx::format("psv_nlong(ctx, {}, {}, {}, {}, {}, {}, {});", op, esize, is_unsigned(in.dt) ? 1 : 0, d.d, n.d, m,
                      scalar ? static_cast<int>(third.index) : -1));
    return true;
}

bool shifts(Emitter& e, const Instruction& in) {
    if (in.op == Op::Vmovl) {
        const V d = vec(in.operands[0]), m = vec(in.operands[1]);
        e.line(nx::format("psv_nshift(ctx, PSV_NS_SHLL, {}, {}, 1, {}, {}, 0);", esize_of(in.dt), is_unsigned(in.dt) ? 1 : 0, d.d, m.d));
        return true;
    }
    if (in.operand_count != 3 || in.operands[2].kind != OperandKind::Imm || !is_vector(in.operands[1])) return false;
    const V d = vec(in.operands[0]), m = vec(in.operands[1]);
    const u32 amount = in.operands[2].imm;
    const char* op = nullptr;
    bool narrow = false;
    switch (in.op) {
        case Op::Vshr: op = "PSV_NS_SHR"; break;
        case Op::Vsra: op = "PSV_NS_SRA"; break;
        case Op::Vrshr: op = "PSV_NS_RSHR"; break;
        case Op::Vrsra: op = "PSV_NS_RSRA"; break;
        case Op::Vsri: op = "PSV_NS_SRI"; break;
        case Op::Vshl: op = "PSV_NS_SHL"; break;
        case Op::Vsli: op = "PSV_NS_SLI"; break;
        case Op::Vqshlu: op = "PSV_NS_QSHLU"; break;
        case Op::Vqshl: op = "PSV_NS_QSHL"; break;
        case Op::Vshrn: op = "PSV_NS_SHRN", narrow = true; break;
        case Op::Vrshrn: op = "PSV_NS_RSHRN", narrow = true; break;
        case Op::Vqshrun: op = "PSV_NS_QSHRUN", narrow = true; break;
        case Op::Vqrshrun: op = "PSV_NS_QRSHRUN", narrow = true; break;
        case Op::Vqshrn: op = "PSV_NS_QSHRN", narrow = true; break;
        case Op::Vqrshrn: op = "PSV_NS_QRSHRN", narrow = true; break;
        case Op::Vshll: op = "PSV_NS_SHLL"; break;
        case Op::Vcvt: {  // between single precision and fixed point
            const bool to_fixed = in.dt == Dt::S32 || in.dt == Dt::U32;
            const Dt fixed = to_fixed ? in.dt : in.dt2;
            e.line(nx::format("psv_ncvt_fixed(ctx, {}, {}, {}, {}, {}, {});", to_fixed ? 1 : 0, fixed == Dt::U32 ? 1 : 0, d.regs, d.d, m.d, amount));
            return true;
        }
        default: return false;
    }
    // (narrowing forms name the wide element type)
    const u32 esize = narrow ? esize_of(in.dt) / 2 : esize_of(in.dt);
    e.line(nx::format("psv_nshift(ctx, {}, {}, {}, {}, {}, {}, {});", op, esize, is_unsigned(in.dt) ? 1 : 0, m.regs, d.d, m.d, amount));
    return true;
}

bool misc(Emitter& e, const Instruction& in) {
    const bool zero_form = in.operand_count == 3 && in.operands[2].kind == OperandKind::Imm && in.operands[2].imm == 0 &&
                           (in.op == Op::Vcgt || in.op == Op::Vcge || in.op == Op::Vceq || in.op == Op::Vcle || in.op == Op::Vclt);
    if (in.operand_count != 2 && !zero_form) return false;
    if (!is_vector(in.operands[0]) || !is_vector(in.operands[1])) return false;
    const V d = vec(in.operands[0]), m = vec(in.operands[1]);
    const bool f = in.dt == Dt::F32;
    const char* op = nullptr;
    bool narrow = false;
    switch (in.op) {
        case Op::Vrev64: op = "PSV_NM_REV64"; break;
        case Op::Vrev32: op = "PSV_NM_REV32"; break;
        case Op::Vrev16: op = "PSV_NM_REV16"; break;
        case Op::Vpaddl: op = "PSV_NM_PADDL"; break;
        case Op::Vpadal: op = "PSV_NM_PADAL"; break;
        case Op::Vcls: op = "PSV_NM_CLS"; break;
        case Op::Vclz: op = "PSV_NM_CLZ"; break;
        case Op::Vcnt: op = "PSV_NM_CNT"; break;
        case Op::Vmvn: op = "PSV_NM_MVN"; break;
        case Op::Vqabs: op = "PSV_NM_QABS"; break;
        case Op::Vqneg: op = "PSV_NM_QNEG"; break;
        case Op::Vcgt: op = f ? "PSV_NM_FCGT0" : "PSV_NM_CGT0"; break;
        case Op::Vcge: op = f ? "PSV_NM_FCGE0" : "PSV_NM_CGE0"; break;
        case Op::Vceq: op = f ? "PSV_NM_FCEQ0" : "PSV_NM_CEQ0"; break;
        case Op::Vcle: op = f ? "PSV_NM_FCLE0" : "PSV_NM_CLE0"; break;
        case Op::Vclt: op = f ? "PSV_NM_FCLT0" : "PSV_NM_CLT0"; break;
        case Op::Vabs: op = f ? "PSV_NM_FABS" : "PSV_NM_ABS"; break;
        case Op::Vneg: op = f ? "PSV_NM_FNEG" : "PSV_NM_NEG"; break;
        case Op::Vrecpe: op = f ? "PSV_NM_FRECPE" : "PSV_NM_RECPE"; break;
        case Op::Vrsqrte: op = f ? "PSV_NM_FRSQRTE" : "PSV_NM_RSQRTE"; break;
        case Op::Vmovn: op = "PSV_NM_MOVN", narrow = true; break;
        case Op::Vqmovun: op = "PSV_NM_QMOVUN", narrow = true; break;
        case Op::Vqmovn: op = "PSV_NM_QMOVN", narrow = true; break;
        case Op::Vswp: e.line(nx::format("psv_nperm(ctx, PSV_NP_SWP, 8, {}, {}, {});", d.regs, d.d, m.d)); return true;
        case Op::Vtrn: e.line(nx::format("psv_nperm(ctx, PSV_NP_TRN, {}, {}, {}, {});", esize_of(in.dt), d.regs, d.d, m.d)); return true;
        case Op::Vuzp: e.line(nx::format("psv_nperm(ctx, PSV_NP_UZP, {}, {}, {}, {});", esize_of(in.dt), d.regs, d.d, m.d)); return true;
        case Op::Vzip: e.line(nx::format("psv_nperm(ctx, PSV_NP_ZIP, {}, {}, {}, {});", esize_of(in.dt), d.regs, d.d, m.d)); return true;
        case Op::Vcvt:
            if (in.dt == Dt::F32 && in.dt2 == Dt::F16) {
                e.line(nx::format("psv_ncvt_half(ctx, 1, {}, {});", d.d, m.d));
                return true;
            }
            if (in.dt == Dt::F16 && in.dt2 == Dt::F32) {
                e.line(nx::format("psv_ncvt_half(ctx, 0, {}, {});", d.d, m.d));
                return true;
            }
            if (in.dt == Dt::S32) op = "PSV_NM_F2S";
            else if (in.dt == Dt::U32) op = "PSV_NM_F2U";
            else if (in.dt2 == Dt::S32) op = "PSV_NM_S2F";
            else op = "PSV_NM_U2F";
            break;
        default: return false;
    }
    u32 esize = esize_of(in.dt);
    if (in.op == Op::Vmvn) esize = 8;
    if (narrow) esize /= 2;
    e.line(nx::format("psv_nmisc(ctx, {}, {}, {}, {}, {}, {});", op, esize, is_unsigned(in.dt) ? 1 : 0, m.regs, d.d, m.d));
    return true;
}

bool other(Emitter& e, const Instruction& in) {
    switch (in.op) {
        case Op::Vext: {
            const V d = vec(in.operands[0]), n = vec(in.operands[1]), m = vec(in.operands[2]);
            e.line(nx::format("psv_next(ctx, {}, {}, {}, {}, {});", d.regs, d.d, n.d, m.d, in.operands[3].imm * esize_of(in.dt) / 8));
            return true;
        }
        case Op::Vtbl:
        case Op::Vtbx: {
            const auto& list = in.operands[1];
            e.line(nx::format("psv_ntbl(ctx, {}, {}, {}, {}, {});", in.op == Op::Vtbx ? 1 : 0, in.operands[0].reg, list.reg, list.count,
                              in.operands[2].reg));
            return true;
        }
        case Op::Vdup: {
            const V d = vec(in.operands[0]);
            const u32 esize = esize_of(in.dt);
            const auto& src = in.operands[1];
            if (src.kind == OperandKind::DScalar)
                e.line(nx::format("psv_ndup(ctx, {}, {}, {}, psv_el(psv_vget(ctx, {}, 1), {}, {}));", esize, d.regs, d.d, src.reg, src.index, esize));
            else
                e.line(nx::format("psv_ndup(ctx, {}, {}, {}, {} & 0x{:X}u);", esize, d.regs, d.d, e.read(in, src.reg),
                                  esize == 32 ? 0xFFFFFFFFu : (1u << esize) - 1));
            return true;
        }
        default: return false;
    }
}

// VLD1-4 / VST1-4: the elements in the pseudocode's order, then the base register's writeback.
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
    const auto load_fn = [&] { return ebytes == 1 ? "psv_ld8" : ebytes == 2 ? "psv_ld16" : ebytes == 4 ? "psv_ld32" : "psv_ld64"; };
    const auto store_fn = [&] { return ebytes == 1 ? "psv_st8" : ebytes == 2 ? "psv_st16" : ebytes == 4 ? "psv_st32" : "psv_st64"; };
    const auto lane = [&](u32 reg, u32 index) {
        // (an element of D register reg as an lvalue of its size)
        if (ebytes == 1) return nx::format("ctx->vfp.b[{}]", reg * 8 + index);
        if (ebytes == 2) return nx::format("ctx->vfp.h[{}]", reg * 4 + index);
        if (ebytes == 4) return nx::format("ctx->vfp.s[{}]", reg * 2 + index);
        return nx::format("ctx->vfp.d[{}]", reg);
    };
    const char* cast = ebytes == 1 ? "(uint8_t)" : ebytes == 2 ? "(uint16_t)" : "";
    u32 size;
    if (list.kind == OperandKind::DRegList) {
        u32 regs, inc;
        if (n == 1) regs = list.count, inc = 1;
        else if (n == 2 && list.count == 4) regs = 2, inc = 2;
        else regs = 1, inc = list.index;
        const u32 elements = 8 / ebytes;
        u32 offset = 0;
        for (u32 r = 0; r < regs; ++r)
            for (u32 el = 0; el < elements; ++el)
                for (u32 i = 0; i < n; ++i) {
                    const u32 reg = list.reg + i * inc + r;
                    if (load) e.line(nx::format("{} = {}{}({} + {}u);", lane(reg, el), cast, load_fn(), base, offset));
                    else e.line(nx::format("{}({} + {}u, {});", store_fn(), base, offset, lane(reg, el)));
                    offset += ebytes;
                }
        size = 8 * list.count;
    } else if (list.flags & arm::kAllLanes) {  // to all lanes (loads only)
        const u32 elements = 8 / ebytes;
        const u32 targets = n == 1 ? list.count : n;
        for (u32 i = 0; i < targets; ++i) {
            const u32 reg = list.reg + i * list.amount;
            const u32 offset = n == 1 ? 0 : i * ebytes;
            const nx::string v = e.temp("v");
            e.line(nx::format("const uint64_t {} = (uint64_t){}({} + {}u);", v, load_fn(), base, offset));
            nx::string all = v;
            if (esize == 8) all = nx::format("{} * 0x0101010101010101ull", v);
            else if (esize == 16) all = nx::format("{} * 0x0001000100010001ull", v);
            else all = nx::format("{} * 0x0000000100000001ull", v);
            (void)elements;
            e.line(nx::format("ctx->vfp.d[{}] = {};", reg, all));
        }
        size = n * ebytes;
    } else {  // one lane
        for (u32 i = 0; i < n; ++i) {
            const u32 reg = list.reg + i * list.amount;
            if (load) e.line(nx::format("{} = {}{}({} + {}u);", lane(reg, list.index), cast, load_fn(), base, i * ebytes));
            else e.line(nx::format("{}({} + {}u, {});", store_fn(), base, i * ebytes, lane(reg, list.index)));
        }
        size = n * ebytes;
    }
    if (m.flags & arm::kMemPostReg) e.line(nx::format("{} = {} + {};", R(m.reg), base, e.read(in, m.reg2)));
    else if (m.flags & arm::kMemWriteBack) e.line(nx::format("{} = {} + {}u;", R(m.reg), base, size));
    return true;
}

}  // namespace

bool emit_simd(Emitter& e, const Instruction& in) {
    return immediate(e, in) || logic(e, in) || memory(e, in) || other(e, in) || long_ops(e, in) || shifts(e, in) || same(e, in) ||
           misc(e, in);
}

}  // namespace psvrecore::recomp::detail
