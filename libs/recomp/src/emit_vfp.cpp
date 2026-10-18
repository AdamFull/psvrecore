// Floating-point instructions (VFPv3): arithmetic, compares, conversions, moves between register files, loads and
// stores, and the FPSCR. The arithmetic is guest.h's helpers, given the context's FPSCR.
//
// The VFP and Advanced SIMD share mnemonics (vadd, vmov, vcvt...). VFP forms name S registers or work on F64;
// Advanced SIMD forms never do (emit_simd.cpp).

#include <bit>

#include "emit.hpp"

namespace psvrecore::recomp::detail {

namespace {

using arm::Dt;

bool is_s(const Operand& o) { return o.kind == OperandKind::SReg; }
bool is_d(const Operand& o) { return o.kind == OperandKind::DReg; }

nx::string vreg(const Operand& o) {
    if (o.kind == OperandKind::SReg) return nx::format("ctx->vfp.s[{}]", o.reg);
    return nx::format("ctx->vfp.d[{}]", o.reg);
}

bool vfp_form(const Instruction& in) {
    if (in.dt == Dt::F64 || in.dt2 == Dt::F64) return true;
    for (u8 i = 0; i < in.operand_count; ++i)
        if (is_s(in.operands[i])) return true;
    return false;
}

const char* suffix(const Instruction& in) { return in.dt == Dt::F64 ? "64" : "32"; }

bool arithmetic(Emitter& e, const Instruction& in) {
    const char* op = nullptr;
    switch (in.op) {
        case Op::Vadd: op = "add"; break;
        case Op::Vsub: op = "sub"; break;
        case Op::Vmul: op = "mul"; break;
        case Op::Vdiv: op = "div"; break;
        default: break;
    }
    const nx::string d = vreg(in.operands[0]);
    const char* w = suffix(in);
    const nx::string sign = in.dt == Dt::F64 ? "0x8000000000000000ull" : "0x80000000u";
    if (op) {
        e.line(nx::format("{} = psv_f{}{}(ctx->fpscr, {}, {});", d, op, w, vreg(in.operands[1]), vreg(in.operands[2])));
        return true;
    }
    // The multiply-accumulates round the product, then the sum (VFPv3 has no fused forms). FPNeg is a sign flip,
    // NaNs included.
    const nx::string product = e.temp("product");
    switch (in.op) {
        case Op::Vmla:
        case Op::Vmls:
        case Op::Vnmla:
        case Op::Vnmls:
        case Op::Vnmul: {
            const char* type = in.dt == Dt::F64 ? "uint64_t" : "uint32_t";
            e.line(nx::format("const {} {} = psv_fmul{}(ctx->fpscr, {}, {});", type, product, w, vreg(in.operands[1]), vreg(in.operands[2])));
            if (in.op == Op::Vnmul) {
                e.line(nx::format("{} = {} ^ {};", d, product, sign));
                return true;
            }
            const nx::string addend = (in.op == Op::Vmla || in.op == Op::Vnmls) ? product : nx::format("({} ^ {})", product, sign);
            const nx::string acc = (in.op == Op::Vmla || in.op == Op::Vmls) ? d : nx::format("({} ^ {})", d, sign);
            e.line(nx::format("{} = psv_fadd{}(ctx->fpscr, {}, {});", d, w, acc, addend));
            return true;
        }
        case Op::Vabs: e.line(nx::format("{} = {} & ~{};", d, vreg(in.operands[1]), sign)); return true;
        case Op::Vneg: e.line(nx::format("{} = {} ^ {};", d, vreg(in.operands[1]), sign)); return true;
        case Op::Vsqrt: e.line(nx::format("{} = psv_fsqrt{}(ctx->fpscr, {});", d, w, vreg(in.operands[1]))); return true;
        case Op::Vcmp:
        case Op::Vcmpe: {
            // (vcmpe also signals on quiet NaNs; exception flags are not computed)
            const nx::string b = in.operands[1].kind == OperandKind::Imm ? nx::string(in.dt == Dt::F64 ? "0ull" : "0u") : vreg(in.operands[1]);
            e.line(nx::format("ctx->fpscr = (ctx->fpscr & 0x0FFFFFFFu) | psv_fcmp{}(ctx->fpscr, {}, {});", w, d, b));
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
            // Between a half in the bottom (vcvtb) or top (vcvtt) half of an S register and single precision.
            const u32 shift = in.op == Op::Vcvtt ? 16 : 0;
            if (in.dt == Dt::F16) {  // to half: the other half of the destination is kept
                const nx::string h = e.temp("h");
                e.line(nx::format("const uint32_t {} = psv_f32_to_f16(ctx->fpscr, {});", h, vreg(m)));
                e.line(nx::format("{0} = ({0} & ~(0xFFFFu << {1})) | ({2} << {1});", vreg(d), shift, h));
            } else {
                e.line(nx::format("{} = psv_f16_to_f32(ctx->fpscr, ({} >> {}) & 0xFFFFu);", vreg(d), vreg(m), shift));
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
        if (is_fixed(in.dt)) {  // to fixed: from F32 (S register) or F64 (D register)
            const char* fn = in.dt2 == Dt::F64 ? "psv_f64_to_fixed" : "psv_f32_to_fixed";
            const nx::string v = nx::format("{}(ctx->fpscr, {}, {}, {}, {})", fn, vreg(m), dt_bits(in.dt), fraction, is_signed_dt(in.dt) ? 1 : 0);
            if (in.dt2 == Dt::F64) e.line(nx::format("{} = (uint64_t){};", vreg(d), is_signed_dt(in.dt) ? nx::format("(int64_t)(int32_t){}", v) : v));
            else e.line(nx::format("{} = {};", vreg(d), v));
        } else {
            const char* fn = in.dt == Dt::F64 ? "psv_fixed_to_f64" : "psv_fixed_to_f32";
            e.line(nx::format("{} = {}(ctx->fpscr, (uint32_t){}, {}, {}, {});", vreg(d), fn, vreg(m), dt_bits(in.dt2), fraction,
                              is_signed_dt(in.dt2) ? 1 : 0));
        }
        return true;
    }
    if (in.dt == Dt::F64 && in.dt2 == Dt::F32) {
        e.line(nx::format("{} = psv_f32_to_f64(ctx->fpscr, {});", vreg(d), vreg(m)));
        return true;
    }
    if (in.dt == Dt::F32 && in.dt2 == Dt::F64) {
        e.line(nx::format("{} = psv_f64_to_f32(ctx->fpscr, {});", vreg(d), vreg(m)));
        return true;
    }
    if (in.dt2 == Dt::S32 || in.dt2 == Dt::U32) {  // from an integer
        e.line(nx::format("{} = psv_int_to_f{}({}, {});", vreg(d), in.dt == Dt::F64 ? "64" : "32", vreg(m), in.dt2 == Dt::S32 ? 1 : 0));
        return true;
    }
    if (in.dt == Dt::S32 || in.dt == Dt::U32) {  // to an integer
        e.line(nx::format("{} = psv_f{}_to_int(ctx->fpscr, {}, {}, {});", vreg(d), in.dt2 == Dt::F64 ? "64" : "32", vreg(m),
                          in.dt == Dt::S32 ? 1 : 0, nearest ? 1 : 0));
        return true;
    }
    return false;
}

bool moves(Emitter& e, const Instruction& in) {
    if (in.op != Op::Vmov) return false;
    const auto& a = in.operands[0];
    const auto& b = in.operands[1];
    if (b.kind == OperandKind::FloatImm) {  // vmov.f32/.f64 #imm
        if (is_s(a)) e.line(nx::format("{} = {};", vreg(a), hex(b.imm)));
        else e.line(nx::format("{} = 0x{:016X}ull;", vreg(a), std::bit_cast<u64>(static_cast<double>(std::bit_cast<float>(b.imm)))));
        return true;
    }
    if (in.operand_count == 2 && (is_s(a) || is_d(a)) && (is_s(b) || is_d(b)) && a.kind == b.kind) {
        e.line(nx::format("{} = {};", vreg(a), vreg(b)));
        return true;
    }
    if (in.operand_count == 2 && a.kind == OperandKind::Reg && is_s(b)) {
        e.write(in, a.reg, vreg(b));
        return true;
    }
    if (in.operand_count == 2 && is_s(a) && b.kind == OperandKind::Reg) {
        e.line(nx::format("{} = {};", vreg(a), e.read(in, b.reg)));
        return true;
    }
    if (in.operand_count == 4 && a.kind == OperandKind::Reg) {  // vmov rt, rt2, sm, sm1
        const nx::string lo = e.temp("lo"), hi = e.temp("hi");
        e.line(nx::format("const uint32_t {} = {};", lo, vreg(in.operands[2])));
        e.line(nx::format("const uint32_t {} = {};", hi, vreg(in.operands[3])));
        e.write(in, a.reg, lo);
        e.write(in, b.reg, hi);
        return true;
    }
    if (in.operand_count == 4 && is_s(a)) {  // vmov sm, sm1, rt, rt2
        const nx::string lo = e.temp("lo"), hi = e.temp("hi");
        e.line(nx::format("const uint32_t {} = {};", lo, e.read(in, in.operands[2].reg)));
        e.line(nx::format("const uint32_t {} = {};", hi, e.read(in, in.operands[3].reg)));
        e.line(nx::format("{} = {};", vreg(a), lo));
        e.line(nx::format("{} = {};", vreg(b), hi));
        return true;
    }
    if (in.operand_count == 3 && a.kind == OperandKind::Reg && is_d(in.operands[2])) {  // vmov rt, rt2, dm
        const nx::string v = e.temp("v");
        e.line(nx::format("const uint64_t {} = {};", v, vreg(in.operands[2])));
        e.write(in, a.reg, nx::format("(uint32_t){}", v));
        e.write(in, b.reg, nx::format("(uint32_t)({} >> 32)", v));
        return true;
    }
    if (in.operand_count == 3 && is_d(a)) {  // vmov dm, rt, rt2
        e.line(nx::format("{} = (uint64_t){} | ((uint64_t){} << 32);", vreg(a), e.read(in, b.reg), e.read(in, in.operands[2].reg)));
        return true;
    }
    // Scalars: vmov.<dt> rt, dn[x] / vmov.<size> dn[x], rt
    if (a.kind == OperandKind::Reg && b.kind == OperandKind::DScalar) {
        const u32 base = b.reg * 8;
        switch (in.dt) {
            case Dt::S8: e.write(in, a.reg, nx::format("(uint32_t)(int32_t)(int8_t)ctx->vfp.b[{}]", base + b.index)); return true;
            case Dt::U8: e.write(in, a.reg, nx::format("(uint32_t)ctx->vfp.b[{}]", base + b.index)); return true;
            case Dt::S16: e.write(in, a.reg, nx::format("(uint32_t)(int32_t)(int16_t)ctx->vfp.h[{}]", b.reg * 4 + b.index)); return true;
            case Dt::U16: e.write(in, a.reg, nx::format("(uint32_t)ctx->vfp.h[{}]", b.reg * 4 + b.index)); return true;
            default: e.write(in, a.reg, nx::format("ctx->vfp.s[{}]", b.reg * 2 + b.index)); return true;
        }
    }
    if (a.kind == OperandKind::DScalar && b.kind == OperandKind::Reg) {
        switch (in.dt) {
            case Dt::B8: e.line(nx::format("ctx->vfp.b[{}] = (uint8_t){};", a.reg * 8 + a.index, e.read(in, b.reg))); return true;
            case Dt::B16: e.line(nx::format("ctx->vfp.h[{}] = (uint16_t){};", a.reg * 4 + a.index, e.read(in, b.reg))); return true;
            default: e.line(nx::format("ctx->vfp.s[{}] = {};", a.reg * 2 + a.index, e.read(in, b.reg))); return true;
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
                    if (dbl) e.line(nx::format("{} = 0x{:08X}{:08X}ull;", vreg(r), hi, lo));
                    else e.line(nx::format("{} = {};", vreg(r), hex(lo)));
                    return true;
                }
            }
            const nx::string a = e.address(in, m, nullptr);
            if (in.op == Op::Vldr) e.line(nx::format("{} = psv_ld{}({});", vreg(r), dbl ? 64 : 32, a));
            else e.line(nx::format("psv_st{}({}, {});", dbl ? 64 : 32, a, vreg(r)));
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
                const nx::string reg = dbl ? nx::format("ctx->vfp.d[{}]", r) : nx::format("ctx->vfp.s[{}]", r);
                const u32 offset = (dbl ? 8 : 4) * i;
                if (load) e.line(nx::format("{} = psv_ld{}({} + {}u);", reg, dbl ? 64 : 32, at, offset));
                else e.line(nx::format("psv_st{}({} + {}u, {});", dbl ? 64 : 32, at, offset, reg));
            }
            if (writeback) e.line(nx::format("{} = {} {} {}u;", R(rn), base, increment ? "+" : "-", bytes));
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
            e.line("ctx->n = ctx->fpscr >> 31;");
            e.line("ctx->z = (ctx->fpscr >> 30) & 1u;");
            e.line("ctx->c = (ctx->fpscr >> 29) & 1u;");
            e.line("ctx->v = (ctx->fpscr >> 28) & 1u;");
            return true;
        }
        e.write(in, in.operands[0].reg, "ctx->fpscr");
        return true;
    }
    if (in.op == Op::Vmsr) {
        if (in.operands[0].imm != kFpscr) {
            e.trap(in, "PSV_TRAP_PRIVILEGED", "vmsr of a system register other than FPSCR");
            return true;
        }
        e.line(nx::format("psv_write_fpscr(ctx, {}, {});", e.read(in, in.operands[1].reg), hex(in.address)));
        return true;
    }
    return false;
}

}  // namespace

bool emit_vfp(Emitter& e, const Instruction& in) {
    if (status(e, in) || load_store(e, in)) return true;
    if (in.op == Op::Vmov && moves(e, in)) return true;
    if (!vfp_form(in)) return false;
    return arithmetic(e, in) || convert(e, in);
}

}  // namespace psvrecore::recomp::detail
