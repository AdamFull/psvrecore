// Floating-point instructions (VFPv3) on the registers in typed locals (analysis/vfp.hpp): s<k> float, d<n> double,
// the FPSCR in `fpscr` and `fpnzcv`. Moves, loads and stores copy bits; the arithmetic, compares and conversions are
// guest.h's typed operations (the host's operation with an exact fallback, or with PSV_NATIVE_FLOAT the host's
// instruction). The same instructions as emit_vfp.cpp's context form, with the same results.

#include <bit>

#include "emit.hpp"

namespace psvrecore::recomp::detail {

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

// An operand's value (float for S, double for D) and its assignment.
nx::string val(Emitter& e, const Operand& o) { return is_s(o) ? e.vs(o.reg) : e.vd(o.reg); }
void set(Emitter& e, const Operand& o, nx::string_view v) {
    if (is_s(o)) e.set_vs(o.reg, v);
    else e.set_vd(o.reg, v);
}

bool arithmetic(Emitter& e, const Instruction& in) {
    const char* w = in.dt == Dt::F64 ? "64" : "32";
    const auto& d = in.operands[0];
    const auto binary = [&](const char* op) {
        const nx::string fpscr = e.fpscr();
        set(e, d, nx::format("psv_v{}{}({}, {}, {})", op, w, fpscr, val(e, in.operands[1]), val(e, in.operands[2])));
    };
    switch (in.op) {
        case Op::Vadd: binary("add"); return true;
        case Op::Vsub: binary("sub"); return true;
        case Op::Vmul: binary("mul"); return true;
        case Op::Vdiv: binary("div"); return true;
        case Op::Vnmul: binary("nmul"); return true;
        case Op::Vmla:
        case Op::Vmls:
        case Op::Vnmla:
        case Op::Vnmls: {
            // VMLA: d + n*m; VMLS: d + -(n*m); VNMLA: -d + -(n*m); VNMLS: -d + n*m. The product is rounded first (VFPv3
            // has no fused forms); negation is FPNeg, a sign flip, NaNs included.
            const char* type = in.dt == Dt::F64 ? "double" : "float";
            const nx::string fpscr = e.fpscr();
            const nx::string product = e.temp("product");
            e.line(nx::format("const {} {} = psv_vmul{}({}, {}, {});", type, product, w, fpscr, val(e, in.operands[1]), val(e, in.operands[2])));
            const nx::string addend = (in.op == Op::Vmla || in.op == Op::Vnmls) ? product : nx::format("psv_vneg{}({})", w, product);
            const nx::string acc = (in.op == Op::Vmla || in.op == Op::Vmls) ? val(e, d) : nx::format("psv_vneg{}({})", w, val(e, d));
            set(e, d, nx::format("psv_vadd{}({}, {}, {})", w, fpscr, acc, addend));
            return true;
        }
        case Op::Vabs: set(e, d, nx::format("psv_vabs{}({})", w, val(e, in.operands[1]))); return true;
        case Op::Vneg: set(e, d, nx::format("psv_vneg{}({})", w, val(e, in.operands[1]))); return true;
        case Op::Vsqrt: {
            const nx::string fpscr = e.fpscr();
            set(e, d, nx::format("psv_vsqrt{}({}, {})", w, fpscr, val(e, in.operands[1])));
            return true;
        }
        case Op::Vcmp:
        case Op::Vcmpe: {
            // (vcmpe also signals on quiet NaNs; exception flags are not computed)
            const nx::string zero = in.dt == Dt::F64 ? "0.0" : "0.0f";
            const nx::string a = val(e, d);
            const nx::string b = in.operands[1].kind == OperandKind::Imm ? zero : val(e, in.operands[1]);
            const nx::string fpscr = e.fpscr();
            if (const u32 width = e.fused_compare()) {
                // (the vmrs after it, and the conditions after that, compare the operands: kept, flushed in FZ mode)
                e.use_fused(width);
                e.line(nx::format("fc{0}a = psv_vflush{0}({1}, {2}); fc{0}b = psv_vflush{0}({1}, {3});", width, fpscr, a, b));
                e.set_fpnzcv(nx::format("psv_vnzcv{0}(fc{0}a, fc{0}b)", width));
                ++e.stats().vfp_fused_compares;
            } else {
                e.set_fpnzcv(nx::format("psv_vcmp{}({}, {}, {})", w, fpscr, a, b));
            }
            return true;
        }
        default: return false;
    }
}

bool is_fixed(Dt dt) { return dt == Dt::S16 || dt == Dt::U16 || dt == Dt::S32 || dt == Dt::U32; }
bool is_signed_dt(Dt dt) { return dt == Dt::S16 || dt == Dt::S32; }
u32 dt_bits(Dt dt) { return dt == Dt::S16 || dt == Dt::U16 ? 16 : 32; }

bool convert(Emitter& e, const Instruction& in) {
    const auto& d = in.operands[0];
    const auto& m = in.operands[1];
    switch (in.op) {
        case Op::Vcvtb:
        case Op::Vcvtt: {
            // Between a half in the bottom (vcvtb) or top (vcvtt) half of an S register and single precision (the
            // integer helpers: rare, and only bits).
            const u32 shift = in.op == Op::Vcvtt ? 16 : 0;
            const nx::string fpscr = e.fpscr();
            if (in.dt == Dt::F16) {  // to half: the other half of the destination is kept
                const nx::string h = e.temp("h");
                e.line(nx::format("const uint32_t {} = psv_f32_to_f16({}, {});", h, fpscr, e.vs_bits(m.reg)));
                e.set_vs_bits(d.reg, nx::format("({} & ~(0xFFFFu << {})) | ({} << {})", e.vs_bits(d.reg), shift, h, shift));
            } else {
                e.set_vs_bits(d.reg, nx::format("psv_f16_to_f32({}, ({} >> {}) & 0xFFFFu)", fpscr, e.vs_bits(m.reg), shift));
            }
            return true;
        }
        case Op::Vcvt:
        case Op::Vcvtr:
            break;
        default: return false;
    }
    const bool nearest = in.op == Op::Vcvtr;
    // Fixed point: {d, d, #fraction}.
    if (in.operand_count == 3) {
        const u32 fraction = in.operands[2].imm;
        const nx::string fpscr = e.fpscr();
        if (is_fixed(in.dt)) {  // to fixed: from F32 (S register) or F64 (D register)
            const bool sgn = is_signed_dt(in.dt);
            if (in.dt2 == Dt::F64) {
                const nx::string v = nx::format("psv_vcvt_fixed_f64({}, {}, {}, {}, {})", fpscr, e.vd(m.reg), dt_bits(in.dt), fraction, sgn ? 1 : 0);
                e.set_vd_bits(d.reg, sgn ? nx::format("(uint64_t)(int64_t)(int32_t){}", v) : nx::format("(uint64_t){}", v));
            } else {
                e.set_vs_bits(d.reg, nx::format("psv_vcvt_fixed_f32({}, {}, {}, {}, {})", fpscr, e.vs(m.reg), dt_bits(in.dt), fraction, sgn ? 1 : 0));
            }
        } else {
            const int sgn = is_signed_dt(in.dt2) ? 1 : 0;
            if (in.dt == Dt::F64)
                e.set_vd(d.reg, nx::format("psv_vcvt_f64_fixed({}, (uint32_t){}, {}, {}, {})", fpscr, e.vd_bits(m.reg), dt_bits(in.dt2), fraction, sgn));
            else
                e.set_vs(d.reg, nx::format("psv_vcvt_f32_fixed({}, {}, {}, {}, {})", fpscr, e.vs_bits(m.reg), dt_bits(in.dt2), fraction, sgn));
        }
        return true;
    }
    if (in.dt == Dt::F64 && in.dt2 == Dt::F32) {
        const nx::string fpscr = e.fpscr();
        e.set_vd(d.reg, nx::format("psv_vcvt_f64_f32({}, {})", fpscr, e.vs(m.reg)));
        return true;
    }
    if (in.dt == Dt::F32 && in.dt2 == Dt::F64) {
        const nx::string fpscr = e.fpscr();
        e.set_vs(d.reg, nx::format("psv_vcvt_f32_f64({}, {})", fpscr, e.vd(m.reg)));
        return true;
    }
    if (in.dt2 == Dt::S32 || in.dt2 == Dt::U32) {  // from an integer (always exact or rounded to nearest)
        const char* from = in.dt2 == Dt::S32 ? "s32" : "u32";
        if (in.dt == Dt::F64) e.set_vd(d.reg, nx::format("psv_vcvt_f64_{}({})", from, e.vs_bits(m.reg)));
        else e.set_vs(d.reg, nx::format("psv_vcvt_f32_{}({})", from, e.vs_bits(m.reg)));
        return true;
    }
    if (in.dt == Dt::S32 || in.dt == Dt::U32) {  // to an integer: toward zero, or (vcvtr) to nearest
        const nx::string fpscr = e.fpscr();
        const nx::string v = in.dt2 == Dt::F64 ? e.vd(m.reg) : e.vs(m.reg);
        e.set_vs_bits(d.reg, nx::format("psv_vcvt{}_{}32_f{}({}, {})", nearest ? "r" : "", in.dt == Dt::S32 ? "s" : "u",
                                        in.dt2 == Dt::F64 ? "64" : "32", fpscr, v));
        return true;
    }
    return false;
}

// A byte or halfword lane of a D register: its word, and the shift of the lane in it.
void lane_of(u32 d, u32 index, u32 bits, u32& word, u32& shift) {
    const u32 bit = d * 64 + index * bits;
    word = bit / 32;
    shift = bit % 32;
}

bool moves(Emitter& e, const Instruction& in) {
    if (in.op != Op::Vmov) return false;
    const auto& a = in.operands[0];
    const auto& b = in.operands[1];
    if (b.kind == OperandKind::FloatImm) {  // vmov.f32/.f64 #imm
        if (is_s(a)) e.set_vs_bits(a.reg, hex(b.imm));
        else e.set_vd_bits(a.reg, nx::format("0x{:016X}ull", std::bit_cast<u64>(static_cast<double>(std::bit_cast<float>(b.imm)))));
        return true;
    }
    if (in.operand_count == 2 && (is_s(a) || is_d(a)) && (is_s(b) || is_d(b)) && a.kind == b.kind) {
        set(e, a, val(e, b));
        return true;
    }
    if (in.operand_count == 2 && a.kind == OperandKind::Reg && is_s(b)) {
        e.write(in, a.reg, e.vs_bits(b.reg));
        return true;
    }
    if (in.operand_count == 2 && is_s(a) && b.kind == OperandKind::Reg) {
        e.set_vs_bits(a.reg, e.read(in, b.reg));
        return true;
    }
    if (in.operand_count == 4 && a.kind == OperandKind::Reg) {  // vmov rt, rt2, sm, sm1
        const nx::string lo = e.temp("lo"), hi = e.temp("hi");
        e.line(nx::format("const uint32_t {} = {};", lo, e.vs_bits(in.operands[2].reg)));
        e.line(nx::format("const uint32_t {} = {};", hi, e.vs_bits(in.operands[3].reg)));
        e.write(in, a.reg, lo);
        e.write(in, b.reg, hi);
        return true;
    }
    if (in.operand_count == 4 && is_s(a)) {  // vmov sm, sm1, rt, rt2
        const nx::string lo = e.temp("lo"), hi = e.temp("hi");
        e.line(nx::format("const uint32_t {} = {};", lo, e.read(in, in.operands[2].reg)));
        e.line(nx::format("const uint32_t {} = {};", hi, e.read(in, in.operands[3].reg)));
        e.set_vs_bits(a.reg, lo);
        e.set_vs_bits(b.reg, hi);
        return true;
    }
    if (in.operand_count == 3 && a.kind == OperandKind::Reg && is_d(in.operands[2])) {  // vmov rt, rt2, dm
        const nx::string v = e.temp("v");
        e.line(nx::format("const uint64_t {} = {};", v, e.vd_bits(in.operands[2].reg)));
        e.write(in, a.reg, nx::format("(uint32_t){}", v));
        e.write(in, b.reg, nx::format("(uint32_t)({} >> 32)", v));
        return true;
    }
    if (in.operand_count == 3 && is_d(a)) {  // vmov dm, rt, rt2
        e.set_vd_bits(a.reg, nx::format("(uint64_t){} | ((uint64_t){} << 32)", e.read(in, b.reg), e.read(in, in.operands[2].reg)));
        return true;
    }
    // Scalars: vmov.<dt> rt, dn[x] / vmov.<size> dn[x], rt (lanes of 8, 16 or 32 bits: a word, or part of one)
    if (a.kind == OperandKind::Reg && b.kind == OperandKind::DScalar) {
        u32 word, shift;
        switch (in.dt) {
            case Dt::S8:
            case Dt::U8:
                lane_of(b.reg, b.index, 8, word, shift);
                if (in.dt == Dt::S8) e.write(in, a.reg, nx::format("(uint32_t)(int32_t)(int8_t)({} >> {})", e.vs_bits(word), shift));
                else e.write(in, a.reg, nx::format("(({} >> {}) & 0xFFu)", e.vs_bits(word), shift));
                return true;
            case Dt::S16:
            case Dt::U16:
                lane_of(b.reg, b.index, 16, word, shift);
                if (in.dt == Dt::S16) e.write(in, a.reg, nx::format("(uint32_t)(int32_t)(int16_t)({} >> {})", e.vs_bits(word), shift));
                else e.write(in, a.reg, nx::format("(({} >> {}) & 0xFFFFu)", e.vs_bits(word), shift));
                return true;
            default: e.write(in, a.reg, e.vs_bits(b.reg * 2 + b.index)); return true;
        }
    }
    if (a.kind == OperandKind::DScalar && b.kind == OperandKind::Reg) {
        u32 word, shift;
        switch (in.dt) {
            case Dt::B8:
            case Dt::B16: {
                const u32 bits = in.dt == Dt::B8 ? 8 : 16;
                const u32 mask = (1u << bits) - 1;
                lane_of(a.reg, a.index, bits, word, shift);
                const nx::string v = e.read(in, b.reg);
                e.set_vs_bits(word, nx::format("({} & ~(0x{:X}u << {})) | (({} & 0x{:X}u) << {})", e.vs_bits(word), mask, shift, v, mask, shift));
                return true;
            }
            default: e.set_vs_bits(a.reg * 2 + a.index, e.read(in, b.reg)); return true;
        }
    }
    return false;
}

bool load_store(Emitter& e, const Instruction& in) {
    switch (in.op) {
        case Op::Vldr:
        case Op::Vstr: {
            const auto& r = in.operands[0];
            const auto& m = in.operands[1];
            const bool dbl = is_d(r);
            if (in.op == Op::Vldr && m.reg == arm::kPc) {  // a literal in a read-only segment
                const u32 base = analysis::pc_value(in) & ~3u;
                const u32 at = (m.flags & arm::kMemSubtract) ? base - m.imm : base + m.imm;
                u32 lo, hi = 0;
                if (e.constant_load(at, 4, lo) && (!dbl || e.constant_load(at + 4, 4, hi))) {
                    ++e.stats().literals_folded;
                    if (dbl) e.set_vd_bits(r.reg, nx::format("0x{:08X}{:08X}ull", hi, lo));
                    else e.set_vs_bits(r.reg, hex(lo));
                    return true;
                }
            }
            const nx::string a = e.address(in, m, nullptr);
            if (in.op == Op::Vldr) {
                if (dbl) e.set_vd_bits(r.reg, nx::format("psv_mld64(mb, {})", a));
                else e.set_vs_bits(r.reg, nx::format("psv_mld32(mb, {})", a));
            } else {
                e.line(nx::format("psv_mst{}(mb, {}, {});", dbl ? 64 : 32, a, dbl ? e.vd_bits(r.reg) : e.vs_bits(r.reg)));
            }
            return true;
        }
        case Op::Vldmia: case Op::Vldmdb: case Op::Vstmia: case Op::Vstmdb: case Op::Vpush: case Op::Vpop: {
            const bool load = in.op == Op::Vldmia || in.op == Op::Vldmdb || in.op == Op::Vpop;
            const bool increment = in.op == Op::Vldmia || in.op == Op::Vstmia || in.op == Op::Vpop;
            const bool push_pop = in.op == Op::Vpush || in.op == Op::Vpop;
            const u32 rn = push_pop ? arm::kSp : in.operands[0].reg;
            const Operand& list = in.operands[push_pop ? 0 : 1];
            const bool writeback = push_pop || (in.flags & arm::kWriteBack);
            const bool dbl = list.kind == OperandKind::DRegList;
            // The bytes the instruction steps over: fldmx/fstmx (odd word counts) include one more word.
            const u32 bytes = (dbl ? 8u : 4u) * list.count + ((in.flags & arm::kXForm) ? 4u : 0u);
            const nx::string base = e.temp("base"), at = e.temp("at");
            e.line(nx::format("const uint32_t {} = {};", base, e.read(in, rn)));
            e.line(nx::format("const uint32_t {} = {}{};", at, base, increment ? nx::string() : nx::format(" - {}u", bytes)));
            for (u32 i = 0; i < list.count; ++i) {
                const u32 r = list.reg + i;
                const u32 offset = (dbl ? 8 : 4) * i;
                if (load) {
                    if (dbl) e.set_vd_bits(r, nx::format("psv_mld64(mb, {} + {}u)", at, offset));
                    else e.set_vs_bits(r, nx::format("psv_mld32(mb, {} + {}u)", at, offset));
                } else {
                    e.line(nx::format("psv_mst{}(mb, {} + {}u, {});", dbl ? 64 : 32, at, offset, dbl ? e.vd_bits(r) : e.vs_bits(r)));
                }
            }
            if (writeback) e.line(e.assignment(rn, nx::format("{} {} {}u", base, increment ? "+" : "-", bytes)));
            return true;
        }
        default: return false;
    }
}

bool status(Emitter& e, const Instruction& in) {
    // Sysreg codes (format.cpp): 5 FPSCR, 11 APSR_nzcv.
    constexpr u32 kFpscr = 5, kApsrNzcv = 11;
    if (in.op == Op::Vmrs) {
        if (in.operands[1].imm != kFpscr) {  // (FPSID, MVFR0/1, FPEXC: not accessible to user code)
            e.trap(in, "PSV_TRAP_PRIVILEGED", "vmrs of a system register other than FPSCR");
            return true;
        }
        if (in.operands[0].kind == OperandKind::Sysreg && in.operands[0].imm == kApsrNzcv) {
            const nx::string nzcv = e.fpnzcv();  // (read, as far as the analysis is concerned, fused or not)
            if (const u32 w = e.fused_vmrs()) {
                // (the compare just before: the flags something reads after the conditions that compare its operands)
                const FlagSet out = e.fused_flags();
                e.use_fused(w);
                if (out & analysis::kFlagN) e.line(nx::format("fl.n = fc{0}a < fc{0}b;", w));
                if (out & analysis::kFlagZ) e.line(nx::format("fl.z = fc{0}a == fc{0}b;", w));
                if (out & analysis::kFlagC) e.line(nx::format("fl.c = !(fc{0}a < fc{0}b);", w));
                if (out & analysis::kFlagV) e.line(nx::format("fl.v = __builtin_isunordered(fc{0}a, fc{0}b);", w));
                return true;
            }
            const FlagSet out = e.live();
            if (out & analysis::kFlagN) e.line(nx::format("fl.n = {} >> 31;", nzcv));
            if (out & analysis::kFlagZ) e.line(nx::format("fl.z = ({} >> 30) & 1u;", nzcv));
            if (out & analysis::kFlagC) e.line(nx::format("fl.c = ({} >> 29) & 1u;", nzcv));
            if (out & analysis::kFlagV) e.line(nx::format("fl.v = ({} >> 28) & 1u;", nzcv));
            return true;
        }
        const nx::string rest = e.fpscr(), nzcv = e.fpnzcv();
        e.write(in, in.operands[0].reg, nx::format("(({} & 0x0FFFFFFFu) | {})", rest, nzcv));
        return true;
    }
    if (in.op == Op::Vmsr) {
        if (in.operands[0].imm != kFpscr) {
            e.trap(in, "PSV_TRAP_PRIVILEGED", "vmsr of a system register other than FPSCR");
            return true;
        }
        // (it traps on a setting not implemented: every local flag in the context first, and the registers on the
        // way to that trap only)
        const nx::string value = e.read(in, in.operands[1].reg);
        e.store_flags_for_trap();
        const nx::string v = e.temp("value");
        e.line(nx::format("const uint32_t {} = {};", v, value));
        if (const nx::string regs = e.trap_register_stores(); !regs.empty())
            e.line(nx::format("if (__builtin_expect(({} & PSV_FPSCR_UNSUPPORTED) != 0u, 0)) {{ {} }}", v, regs));
        e.set_fpscr(nx::format("psv_vmsr(ctx, {}, {})", v, hex(in.address)));
        return true;
    }
    return false;
}

}  // namespace

bool emit_vfp_locals(Emitter& e, const Instruction& in) {
    if (status(e, in) || load_store(e, in)) return true;
    if (in.op == Op::Vmov && moves(e, in)) return true;
    if (!vfp_form(in)) return false;
    return arithmetic(e, in) || convert(e, in);
}

}  // namespace psvrecore::recomp::detail
