// Integer instructions: data processing, multiplies, saturation, parallel arithmetic, extension and bit fields,
// branches, and the system instructions user code can execute.

#include "emit.hpp"

namespace psvrecore::recomp::detail {

namespace {

// The value of a register operand rotated right (sxtb r0, r1, ror #8), or plain.
nx::string rotated(Emitter& e, const Instruction& in, const Operand& o) {
    const nx::string v = e.read(in, o.reg);
    if (o.kind == OperandKind::RegShiftImm && o.amount) return nx::format("psv_ror({}, {})", v, o.amount);
    return v;
}

enum class Arith { None, Add, Adc, Sub, Sbc, Rsb, Rsc };
enum class Logic { None, And, Eor, Orr, Bic, Orn, Mov, Mvn };

bool data_processing(Emitter& e, const Instruction& in) {
    Arith arith = Arith::None;
    Logic logic = Logic::None;
    bool compare = false;  // no destination
    switch (in.op) {
        case Op::Add: arith = Arith::Add; break;
        case Op::Adc: arith = Arith::Adc; break;
        case Op::Sub: arith = Arith::Sub; break;
        case Op::Sbc: arith = Arith::Sbc; break;
        case Op::Rsb: arith = Arith::Rsb; break;
        case Op::Rsc: arith = Arith::Rsc; break;
        case Op::Cmp: arith = Arith::Sub, compare = true; break;
        case Op::Cmn: arith = Arith::Add, compare = true; break;
        case Op::And: logic = Logic::And; break;
        case Op::Eor: logic = Logic::Eor; break;
        case Op::Orr: logic = Logic::Orr; break;
        case Op::Bic: logic = Logic::Bic; break;
        case Op::Orn: logic = Logic::Orn; break;
        case Op::Mov: logic = Logic::Mov; break;
        case Op::Mvn: logic = Logic::Mvn; break;
        case Op::Tst: logic = Logic::And, compare = true; break;
        case Op::Teq: logic = Logic::Eor, compare = true; break;
        default: return false;
    }
    const bool s = compare || (in.flags & arm::kSetsFlags);
    // Operand shapes: {rd, rn, op2}, {rdn, op2} (Thumb two-operand forms), {rn, op2} (compares), {rd, op2} (mov, mvn).
    u32 rd = 0, rn = 0;
    const Operand* op2;
    const bool unary = logic == Logic::Mov || logic == Logic::Mvn;
    if (compare) {
        rn = in.operands[0].reg;
        op2 = &in.operands[1];
    } else if (unary) {
        rd = in.operands[0].reg;
        op2 = &in.operands[1];
    } else if (in.operand_count == 2) {
        rd = rn = in.operands[0].reg;
        op2 = &in.operands[1];
    } else {
        rd = in.operands[0].reg;
        rn = in.operands[1].reg;
        op2 = &in.operands[2];
    }
    if (!compare && rd == arm::kPc && (in.flags & arm::kSetsFlags)) {  // (an exception return)
        e.trap(in, "PSV_TRAP_PRIVILEGED", nx::format("{}s pc", arm::mnemonic(in.op)));
        return true;
    }
    const bool need_carry = s && logic != Logic::None;
    nx::string carry;
    const nx::string a = e.temp("a"), b = e.temp("b"), r = e.temp("r");
    if (!unary) e.line(nx::format("const uint32_t {} = {};", a, e.read(in, rn)));
    e.line(nx::format("const uint32_t {} = {};", b, e.operand2(in, *op2, need_carry ? &carry : nullptr)));
    if (arith != Arith::None) {
        nx::string x = a, y = b, cin;
        switch (arith) {
            case Arith::Add: cin = "0u"; break;
            case Arith::Adc: cin = "ctx->c"; break;
            case Arith::Sub: y = "~" + b, cin = "1u"; break;
            case Arith::Sbc: y = "~" + b, cin = "ctx->c"; break;
            case Arith::Rsb: x = b, y = "~" + a, cin = "1u"; break;
            default: x = b, y = "~" + a, cin = "ctx->c"; break;  // rsc
        }
        if (s) e.line(nx::format("const uint32_t {} = psv_adds(ctx, {}, {}, {});", r, x, y, cin));
        else e.line(nx::format("const uint32_t {} = {} + {} + {};", r, x, y, cin));
    } else {
        nx::string value;
        switch (logic) {
            case Logic::And: value = a + " & " + b; break;
            case Logic::Eor: value = a + " ^ " + b; break;
            case Logic::Orr: value = a + " | " + b; break;
            case Logic::Bic: value = a + " & ~" + b; break;
            case Logic::Orn: value = a + " | ~" + b; break;
            case Logic::Mov: value = b; break;
            default: value = "~" + b; break;
        }
        e.line(nx::format("const uint32_t {} = {};", r, value));
        if (s) {
            e.line(nx::format("psv_nz(ctx, {});", r));
            if (carry != "ctx->c") e.line(nx::format("ctx->c = {};", carry));
        }
    }
    if (!compare) e.write(in, rd, r, Emitter::PcWrite::Branch);
    return true;
}

// lsl, lsr, asr, ror, rrx: {rd, rm, #imm}, {rd, rm, rs}, or {rdn, rm} (Thumb: rdn shifted by rm).
bool shift(Emitter& e, const Instruction& in) {
    u32 type;
    switch (in.op) {
        case Op::Lsl: type = 0; break;
        case Op::Lsr: type = 1; break;
        case Op::Asr: type = 2; break;
        case Op::Ror: type = 3; break;
        case Op::Rrx: type = 4; break;
        default: return false;
    }
    const bool s = in.flags & arm::kSetsFlags;
    const u32 rd = in.operands[0].reg;
    const nx::string x = e.temp("x"), r = e.temp("r");
    nx::string value, carry;
    if (type == 4) {
        e.line(nx::format("const uint32_t {} = {};", x, e.read(in, in.operands[1].reg)));
        value = nx::format("(ctx->c << 31) | ({} >> 1)", x);
        carry = x + " & 1u";
    } else if (in.operand_count == 3 && in.operands[2].kind == OperandKind::Imm) {
        e.line(nx::format("const uint32_t {} = {};", x, e.read(in, in.operands[1].reg)));
        const u32 n = in.operands[2].imm;
        switch (type) {
            case 0: value = nx::format("{} << {}", x, n), carry = nx::format("({} >> {}) & 1u", x, 32 - n); break;
            case 1:
                value = n == 32 ? nx::string("0u") : nx::format("{} >> {}", x, n);
                carry = nx::format("({} >> {}) & 1u", x, n - 1);
                break;
            case 2:
                value = nx::format("psv_asr({}, {})", x, n);
                carry = nx::format("({} >> {}) & 1u", x, n == 32 ? 31 : n - 1);
                break;
            default: value = nx::format("psv_ror({}, {})", x, n), carry = nx::format("({} >> {}) & 1u", x, n - 1); break;
        }
    } else {
        const u32 rm = in.operand_count == 3 ? in.operands[1].reg : in.operands[0].reg;
        const u32 rs = in.operand_count == 3 ? in.operands[2].reg : in.operands[1].reg;
        const nx::string amount = e.temp("s");
        e.line(nx::format("const uint32_t {} = {};", x, e.read(in, rm)));
        e.line(nx::format("const uint32_t {} = {};", amount, e.read(in, rs)));
        value = nx::format("psv_shift_reg({}, {}, {})", x, type, amount);
        carry = nx::format("psv_shift_reg_c({}, {}, {}, ctx->c)", x, type, amount);
    }
    e.line(nx::format("const uint32_t {} = {};", r, value));
    if (s) {
        e.line(nx::format("ctx->c = {};", carry));
        e.line(nx::format("psv_nz(ctx, {});", r));
    }
    e.write(in, rd, r, Emitter::PcWrite::Branch);
    return true;
}

bool multiply(Emitter& e, const Instruction& in) {
    const auto rr = [&](int i) { return e.read(in, in.operands[i].reg); };
    const auto reg = [&](int i) { return in.operands[i].reg; };
    const bool s = in.flags & arm::kSetsFlags;
    const nx::string r = e.temp("r");
    switch (in.op) {
        case Op::Mul:
        case Op::Mla:
        case Op::Mls: {
            nx::string value = nx::format("{} * {}", rr(1), rr(2));
            if (in.op == Op::Mla) value = nx::format("{} * {} + {}", rr(1), rr(2), rr(3));
            if (in.op == Op::Mls) value = nx::format("{} - {} * {}", rr(3), rr(1), rr(2));
            e.line(nx::format("const uint32_t {} = {};", r, value));
            if (s) e.line(nx::format("psv_nz(ctx, {});", r));
            e.write(in, reg(0), r);
            return true;
        }
        case Op::Umull:
        case Op::Umlal:
        case Op::Smull:
        case Op::Smlal:
        case Op::Umaal: {
            // {lo, hi, rn, rm}
            nx::string product;
            if (in.op == Op::Smull || in.op == Op::Smlal) product = nx::format("(uint64_t)((int64_t)(int32_t){} * (int32_t){})", rr(2), rr(3));
            else product = nx::format("(uint64_t){} * {}", rr(2), rr(3));
            const nx::string acc = nx::format("(((uint64_t){} << 32) | {})", rr(1), rr(0));
            nx::string value = product;
            if (in.op == Op::Umlal || in.op == Op::Smlal) value = product + " + " + acc;
            if (in.op == Op::Umaal) value = nx::format("{} + {} + {}", product, rr(0), rr(1));
            e.line(nx::format("const uint64_t {} = {};", r, value));
            if (s) {
                e.line(nx::format("ctx->n = (uint32_t)({} >> 63);", r));
                e.line(nx::format("ctx->z = {} == 0;", r));
            }
            e.write(in, reg(0), nx::format("(uint32_t){}", r));
            e.write(in, reg(1), nx::format("(uint32_t)({} >> 32)", r));
            return true;
        }
        default: break;
    }
    // Halfword, word-by-halfword, dual and most-significant-word multiplies.
    const auto half = [&](int i, bool top) { return nx::format("psv_half({}, {})", rr(i), top ? 1 : 0); };
    switch (in.op) {
        case Op::Smulbb: case Op::Smulbt: case Op::Smultb: case Op::Smultt: {
            const bool nt = in.op == Op::Smultb || in.op == Op::Smultt, mt = in.op == Op::Smulbt || in.op == Op::Smultt;
            e.write(in, reg(0), nx::format("(uint32_t)({} * {})", half(1, nt), half(2, mt)));
            return true;
        }
        case Op::Smlabb: case Op::Smlabt: case Op::Smlatb: case Op::Smlatt: {
            const bool nt = in.op == Op::Smlatb || in.op == Op::Smlatt, mt = in.op == Op::Smlabt || in.op == Op::Smlatt;
            e.write(in, reg(0), nx::format("psv_add_q(ctx, (int64_t)({} * {}) + (int32_t){})", half(1, nt), half(2, mt), rr(3)));
            return true;
        }
        case Op::Smulwb: case Op::Smulwt:
            e.write(in, reg(0), nx::format("(uint32_t)(((int64_t)(int32_t){} * {}) >> 16)", rr(1), half(2, in.op == Op::Smulwt)));
            return true;
        case Op::Smlawb: case Op::Smlawt:
            e.write(in, reg(0), nx::format("psv_add_q(ctx, (((int64_t)(int32_t){} * {}) >> 16) + (int32_t){})", rr(1),
                                           half(2, in.op == Op::Smlawt), rr(3)));
            return true;
        case Op::Smlalbb: case Op::Smlalbt: case Op::Smlaltb: case Op::Smlaltt: {
            const bool nt = in.op == Op::Smlaltb || in.op == Op::Smlaltt, mt = in.op == Op::Smlalbt || in.op == Op::Smlaltt;
            e.line(nx::format("const uint64_t {} = (((uint64_t){} << 32) | {}) + (uint64_t)(int64_t)({} * {});", r, rr(1), rr(0),
                              half(2, nt), half(3, mt)));
            e.write(in, reg(0), nx::format("(uint32_t){}", r));
            e.write(in, reg(1), nx::format("(uint32_t)({} >> 32)", r));
            return true;
        }
        case Op::Smuad: case Op::Smuadx: case Op::Smusd: case Op::Smusdx:
        case Op::Smlad: case Op::Smladx: case Op::Smlsd: case Op::Smlsdx: {
            const bool x = in.op == Op::Smuadx || in.op == Op::Smusdx || in.op == Op::Smladx || in.op == Op::Smlsdx;
            const bool sub = in.op == Op::Smusd || in.op == Op::Smusdx || in.op == Op::Smlsd || in.op == Op::Smlsdx;
            const bool acc = in.op == Op::Smlad || in.op == Op::Smladx || in.op == Op::Smlsd || in.op == Op::Smlsdx;
            const nx::string p1 = nx::format("(int64_t)({} * {})", half(1, false), half(2, x));
            const nx::string p2 = nx::format("(int64_t)({} * {})", half(1, true), half(2, !x));
            nx::string sum = nx::format("{} {} {}", p1, sub ? "-" : "+", p2);
            if (acc) sum += nx::format(" + (int32_t){}", rr(3));
            e.write(in, reg(0), nx::format("psv_add_q(ctx, {})", sum));
            return true;
        }
        case Op::Smlald: case Op::Smlaldx: case Op::Smlsld: case Op::Smlsldx: {
            // {lo, hi, rn, rm}
            const bool x = in.op == Op::Smlaldx || in.op == Op::Smlsldx, sub = in.op == Op::Smlsld || in.op == Op::Smlsldx;
            const nx::string p1 = nx::format("(int64_t)({} * {})", half(2, false), half(3, x));
            const nx::string p2 = nx::format("(int64_t)({} * {})", half(2, true), half(3, !x));
            e.line(nx::format("const uint64_t {} = (((uint64_t){} << 32) | {}) + (uint64_t)({} {} {});", r, rr(1), rr(0), p1,
                              sub ? "-" : "+", p2));
            e.write(in, reg(0), nx::format("(uint32_t){}", r));
            e.write(in, reg(1), nx::format("(uint32_t)({} >> 32)", r));
            return true;
        }
        case Op::Smmul: case Op::Smmulr: case Op::Smmla: case Op::Smmlar: case Op::Smmls: case Op::Smmlsr: {
            const bool round = in.op == Op::Smmulr || in.op == Op::Smmlar || in.op == Op::Smmlsr;
            // (modulo 2^64: bits 63-32 of the 65-bit sum are the same)
            const nx::string product = nx::format("(uint64_t)((int64_t)(int32_t){} * (int32_t){})", rr(1), rr(2));
            nx::string value;
            if (in.op == Op::Smmul || in.op == Op::Smmulr) value = product;
            else if (in.op == Op::Smmla || in.op == Op::Smmlar) value = nx::format("((uint64_t){} << 32) + {}", rr(3), product);
            else value = nx::format("((uint64_t){} << 32) - {}", rr(3), product);
            if (round) value = nx::format("{} + 0x80000000u", value);
            e.write(in, reg(0), nx::format("(uint32_t)(({}) >> 32)", value));
            return true;
        }
        case Op::Usad8: e.write(in, reg(0), nx::format("psv_usad8({}, {})", rr(1), rr(2))); return true;
        case Op::Usada8: e.write(in, reg(0), nx::format("psv_usad8({}, {}) + {}", rr(1), rr(2), rr(3))); return true;
        default: return false;
    }
}

bool parallel(Emitter& e, const Instruction& in) {
    // kind: 0 s, 1 q, 2 sh, 3 u, 4 uq, 5 uh; op: 0 add16, 1 asx, 2 sax, 3 sub16, 4 add8, 5 sub8
    struct P {
        Op op;
        int kind, which;
    };
    static constexpr P kTable[] = {
        {Op::Sadd16, 0, 0}, {Op::Sasx, 0, 1}, {Op::Ssax, 0, 2}, {Op::Ssub16, 0, 3}, {Op::Sadd8, 0, 4}, {Op::Ssub8, 0, 5},
        {Op::Qadd16, 1, 0}, {Op::Qasx, 1, 1}, {Op::Qsax, 1, 2}, {Op::Qsub16, 1, 3}, {Op::Qadd8, 1, 4}, {Op::Qsub8, 1, 5},
        {Op::Shadd16, 2, 0}, {Op::Shasx, 2, 1}, {Op::Shsax, 2, 2}, {Op::Shsub16, 2, 3}, {Op::Shadd8, 2, 4}, {Op::Shsub8, 2, 5},
        {Op::Uadd16, 3, 0}, {Op::Uasx, 3, 1}, {Op::Usax, 3, 2}, {Op::Usub16, 3, 3}, {Op::Uadd8, 3, 4}, {Op::Usub8, 3, 5},
        {Op::Uqadd16, 4, 0}, {Op::Uqasx, 4, 1}, {Op::Uqsax, 4, 2}, {Op::Uqsub16, 4, 3}, {Op::Uqadd8, 4, 4}, {Op::Uqsub8, 4, 5},
        {Op::Uhadd16, 5, 0}, {Op::Uhasx, 5, 1}, {Op::Uhsax, 5, 2}, {Op::Uhsub16, 5, 3}, {Op::Uhadd8, 5, 4}, {Op::Uhsub8, 5, 5},
    };
    for (const auto& p : kTable)
        if (p.op == in.op) {
            e.write(in, in.operands[0].reg, nx::format("psv_parallel(ctx, {}, {}, {}, {})", e.read(in, in.operands[1].reg),
                                                       e.read(in, in.operands[2].reg), p.kind, p.which));
            return true;
        }
    return false;
}

bool misc(Emitter& e, const Instruction& in) {
    const auto rr = [&](int i) { return e.read(in, in.operands[i].reg); };
    const u32 rd = in.operands[0].reg;
    switch (in.op) {
        case Op::Movw: e.write(in, rd, hex(in.operands[1].imm)); return true;
        case Op::Movt: e.write(in, rd, nx::format("({} & 0xFFFFu) | {}", rr(0), hex(in.operands[1].imm << 16))); return true;
        case Op::Adr: e.write(in, rd, hex(((analysis::pc_value(in)) & ~3u) + in.operands[1].imm)); return true;
        case Op::Clz: e.write(in, rd, nx::format("psv_clz({})", rr(1))); return true;
        case Op::Rbit: e.write(in, rd, nx::format("psv_rbit({})", rr(1))); return true;
        case Op::Rev: e.write(in, rd, nx::format("__builtin_bswap32({})", rr(1))); return true;
        case Op::Rev16: e.write(in, rd, nx::format("psv_rev16({})", rr(1))); return true;
        case Op::Revsh: e.write(in, rd, nx::format("psv_revsh({})", rr(1))); return true;
        case Op::Sel: e.write(in, rd, nx::format("psv_sel(ctx, {}, {})", rr(1), rr(2))); return true;
        case Op::Qadd: e.write(in, rd, nx::format("psv_qadd(ctx, {}, {})", rr(1), rr(2))); return true;
        case Op::Qsub: e.write(in, rd, nx::format("psv_qsub(ctx, {}, {})", rr(1), rr(2))); return true;
        case Op::Qdadd: e.write(in, rd, nx::format("psv_qdadd(ctx, {}, {})", rr(1), rr(2))); return true;
        case Op::Qdsub: e.write(in, rd, nx::format("psv_qdsub(ctx, {}, {})", rr(1), rr(2))); return true;
        case Op::Ssat:
        case Op::Usat: {
            const nx::string v = e.temp("v");
            e.line(nx::format("const uint32_t {} = {};", v, e.operand2(in, in.operands[2], nullptr)));
            e.write(in, rd, nx::format("{}(ctx, (int32_t){}, {})", in.op == Op::Ssat ? "psv_ssat" : "psv_usat", v, in.operands[1].imm));
            return true;
        }
        case Op::Ssat16: e.write(in, rd, nx::format("psv_ssat16(ctx, {}, {})", rr(2), in.operands[1].imm)); return true;
        case Op::Usat16: e.write(in, rd, nx::format("psv_usat16(ctx, {}, {})", rr(2), in.operands[1].imm)); return true;
        case Op::Sxtb: e.write(in, rd, nx::format("(uint32_t)(int32_t)(int8_t){}", rotated(e, in, in.operands[1]))); return true;
        case Op::Sxth: e.write(in, rd, nx::format("(uint32_t)(int32_t)(int16_t){}", rotated(e, in, in.operands[1]))); return true;
        case Op::Uxtb: e.write(in, rd, nx::format("({} & 0xFFu)", rotated(e, in, in.operands[1]))); return true;
        case Op::Uxth: e.write(in, rd, nx::format("({} & 0xFFFFu)", rotated(e, in, in.operands[1]))); return true;
        case Op::Sxtab: e.write(in, rd, nx::format("{} + (uint32_t)(int32_t)(int8_t){}", rr(1), rotated(e, in, in.operands[2]))); return true;
        case Op::Sxtah: e.write(in, rd, nx::format("{} + (uint32_t)(int32_t)(int16_t){}", rr(1), rotated(e, in, in.operands[2]))); return true;
        case Op::Uxtab: e.write(in, rd, nx::format("{} + ({} & 0xFFu)", rr(1), rotated(e, in, in.operands[2]))); return true;
        case Op::Uxtah: e.write(in, rd, nx::format("{} + ({} & 0xFFFFu)", rr(1), rotated(e, in, in.operands[2]))); return true;
        case Op::Sxtb16:
        case Op::Uxtb16:
        case Op::Sxtab16:
        case Op::Uxtab16: {
            const bool acc = in.op == Op::Sxtab16 || in.op == Op::Uxtab16;
            const bool sign = in.op == Op::Sxtb16 || in.op == Op::Sxtab16;
            const nx::string x = e.temp("x");
            e.line(nx::format("const uint32_t {} = {};", x, rotated(e, in, in.operands[acc ? 2 : 1])));
            nx::string lo = sign ? nx::format("(uint32_t)(int32_t)(int8_t){}", x) : nx::format("({} & 0xFFu)", x);
            nx::string hi = sign ? nx::format("(uint32_t)(int32_t)(int8_t)({} >> 16)", x) : nx::format("(({} >> 16) & 0xFFu)", x);
            if (acc) {
                const nx::string n = e.temp("n");
                e.line(nx::format("const uint32_t {} = {};", n, rr(1)));
                lo = nx::format("({} + {})", n, lo);
                hi = nx::format("(({} >> 16) + {})", n, hi);
            }
            e.write(in, rd, nx::format("({} & 0xFFFFu) | ({} << 16)", lo, hi));
            return true;
        }
        case Op::Pkhbt:
        case Op::Pkhtb: {
            const nx::string m = e.temp("m");
            e.line(nx::format("const uint32_t {} = {};", m, e.operand2(in, in.operands[2], nullptr)));
            if (in.op == Op::Pkhbt) e.write(in, rd, nx::format("({} & 0xFFFFu) | ({} & 0xFFFF0000u)", rr(1), m));
            else e.write(in, rd, nx::format("({} & 0xFFFF0000u) | ({} & 0xFFFFu)", rr(1), m));
            return true;
        }
        case Op::Bfc: {
            const u32 lsb = in.operands[1].imm, width = in.operands[2].imm;
            const u32 mask = (width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1)) << lsb;
            e.write(in, rd, nx::format("{} & {}", rr(0), hex(~mask)));
            return true;
        }
        case Op::Bfi: {
            const u32 lsb = in.operands[2].imm, width = in.operands[3].imm;
            const u32 mask = (width >= 32 ? 0xFFFFFFFFu : ((1u << width) - 1)) << lsb;
            e.write(in, rd, nx::format("({} & {}) | (({} << {}) & {})", rr(0), hex(~mask), rr(1), lsb, hex(mask)));
            return true;
        }
        case Op::Sbfx:
        case Op::Ubfx: {
            const u32 lsb = in.operands[2].imm, width = in.operands[3].imm;
            if (lsb + width > 32) return false;  // (unpredictable; flagged by the decoder)
            if (in.op == Op::Ubfx)
                e.write(in, rd, nx::format("({} >> {}) & {}", rr(1), lsb, hex(width >= 32 ? 0xFFFFFFFFu : (1u << width) - 1)));
            else
                e.write(in, rd, nx::format("(uint32_t)((int32_t)({} << {}) >> {})", rr(1), 32 - lsb - width, 32 - width));
            return true;
        }
        default: return false;
    }
}

bool branches(Emitter& e, const Instruction& in) {
    switch (in.op) {
        case Op::B: {
            const auto f = analysis::flow_of(in);
            e.branch_to(in, f.target);
            return true;
        }
        case Op::Cbz:
        case Op::Cbnz: {
            const auto f = analysis::flow_of(in);
            e.line(nx::format("if ({} {} 0) {{", R(in.operands[0].reg), in.op == Op::Cbz ? "==" : "!="));
            e.branch_to(in, f.target);
            e.line("}");
            return true;
        }
        case Op::Bl:
        case Op::Blx:
            if (in.operands[0].kind == OperandKind::Label) {
                e.call(in, analysis::flow_of(in).target);
            } else {
                const nx::string t = e.temp("target");
                e.line(nx::format("const uint32_t {} = {};", t, e.read(in, in.operands[0].reg)));
                e.line(nx::format("{} = {};", R(arm::kLr), hex(e.next_code(in))));
                e.line(nx::format("psv_call(ctx, {}, {});", t, hex(e.next_code(in))));
                ++e.stats().indirect_calls;
            }
            return true;
        case Op::Bx:
        case Op::Bxj:  // (Jazelle is not implemented: bxj behaves as bx)
            e.branch_to_value(in, e.read(in, in.operands[0].reg));
            return true;
        case Op::Tbb:
        case Op::Tbh: {
            // The target is the PC (address + 4) plus twice the table entry, in Thumb.
            const auto& m = in.operands[0];
            const nx::string index = e.temp("index");
            e.line(nx::format("const uint32_t {} = {};", index, e.read(in, m.reg2)));
            const nx::string entry = in.op == Op::Tbb ? nx::format("psv_ld8({} + {})", e.read(in, m.reg), index)
                                                      : nx::format("psv_ld16({} + ({} << 1))", e.read(in, m.reg), index);
            e.branch_to_value(in, nx::format("({} + 2u * {}) | 1u", hex(analysis::pc_value(in)), entry));
            return true;
        }
        default: return false;
    }
}

bool system(Emitter& e, const Instruction& in) {
    switch (in.op) {
        case Op::It:
        case Op::Nop:
        case Op::Hint:
        case Op::Dbg:
        case Op::Sev:
        case Op::Isb:
        case Op::Pld:
        case Op::Pldw:
        case Op::Pli:
            return true;
        case Op::Yield:
        case Op::Wfe:
        case Op::Wfi:
            e.line("psv_yield(ctx);");
            return true;
        case Op::Dmb:
        case Op::Dsb:
            e.line("psv_barrier();");
            return true;
        case Op::Clrex:
            e.line("ctx->excl_size = 0;");
            return true;
        case Op::Svc:
            e.line(nx::format("psv_svc(ctx, {}, {});", hex(in.address), hex(in.operands[0].imm)));
            return true;
        case Op::Bkpt: e.trap(in, "PSV_TRAP_BKPT", "bkpt"); return true;
        case Op::Udf: e.trap(in, "PSV_TRAP_UDF", "udf"); return true;
        case Op::Setend:
            if (in.operands[0].imm == 2) return true;  // (setend le: the only state)
            e.trap(in, "PSV_TRAP_BIG_ENDIAN", "setend be");
            return true;
        case Op::Mrs:
            if (in.operands[1].imm != 1) {  // (SPSR: privileged)
                e.trap(in, "PSV_TRAP_PRIVILEGED", "mrs spsr");
                return true;
            }
            e.write(in, in.operands[0].reg, "psv_read_apsr(ctx)");
            return true;
        case Op::Msr: {
            const u32 code = in.operands[0].imm;
            if (code & 0x10) {
                e.trap(in, "PSV_TRAP_PRIVILEGED", "msr spsr");
                return true;
            }
            const auto& src = in.operands[1];
            const nx::string value = src.kind == OperandKind::Reg ? e.read(in, src.reg) : hex(src.imm);
            // (in user mode only the flags and GE are written; the other fields are ignored)
            e.line(nx::format("psv_write_apsr(ctx, {}, {});", value, code & 0xF));
            return true;
        }
        case Op::Mrc:
        case Op::Mcr: {
            // CP15 c13: the software thread ID registers user code may use.
            const u32 cp = in.operands[0].imm, opc1 = in.operands[1].imm, crn = in.operands[3].reg, crm = in.operands[4].reg,
                      opc2 = in.operands[5].imm;
            if (cp == 15 && opc1 == 0 && crn == 13 && crm == 0 && (opc2 == 2 || opc2 == 3) && in.operands[2].kind == OperandKind::Reg) {
                const u32 rt = in.operands[2].reg;
                const char* field = opc2 == 2 ? "ctx->tpidrurw" : "ctx->tpidruro";
                if (in.op == Op::Mrc) {
                    e.write(in, rt, field);
                    return true;
                }
                if (opc2 == 2) {
                    e.line(nx::format("{} = {};", field, e.read(in, rt)));
                    return true;
                }
            }
            e.trap(in, "PSV_TRAP_PRIVILEGED", nx::format("{} p{}", arm::mnemonic(in.op), cp));
            return true;
        }
        case Op::Cps:
        case Op::Cpsie:
        case Op::Cpsid:
        case Op::Smc:
        case Op::Subs:
        case Op::Eret:
        case Op::Srsda: case Op::Srsdb: case Op::Srsia: case Op::Srsib:
        case Op::Rfeda: case Op::Rfedb: case Op::Rfeia: case Op::Rfeib:
        case Op::Cdp: case Op::Cdp2: case Op::Mcr2: case Op::Mrc2: case Op::Mcrr: case Op::Mcrr2: case Op::Mrrc: case Op::Mrrc2:
        case Op::Ldc: case Op::Ldcl: case Op::Ldc2: case Op::Ldc2l: case Op::Stc: case Op::Stcl: case Op::Stc2: case Op::Stc2l:
            e.trap(in, "PSV_TRAP_PRIVILEGED", arm::mnemonic(in.op));
            return true;
        default: return false;
    }
}

}  // namespace

nx::string Emitter::operand2(const Instruction& in, const Operand& o, nx::string* carry) {
    const bool want = carry != nullptr;
    switch (o.kind) {
        case OperandKind::Imm:
        case OperandKind::ModImm:
            if (want) *carry = (in.flags & arm::kImmCarry) ? nx::string((o.imm >> 31) ? "1u" : "0u") : nx::string("ctx->c");
            return hex(o.imm);
        case OperandKind::Reg:
            if (want) *carry = "ctx->c";
            return read(in, o.reg);
        case OperandKind::RegShiftImm: {
            const nx::string x = temp("x");
            line(nx::format("const uint32_t {} = {};", x, read(in, o.reg)));
            const u32 n = o.amount;
            switch (o.shift) {
                case arm::ShiftType::Lsl:
                    if (want) *carry = nx::format("({} >> {}) & 1u", x, 32 - n);
                    return nx::format("({} << {})", x, n);
                case arm::ShiftType::Lsr:
                    if (want) *carry = nx::format("({} >> {}) & 1u", x, n - 1);
                    return n == 32 ? nx::string("0u") : nx::format("({} >> {})", x, n);
                case arm::ShiftType::Asr:
                    if (want) *carry = nx::format("({} >> {}) & 1u", x, n == 32 ? 31 : n - 1);
                    return nx::format("psv_asr({}, {})", x, n);
                case arm::ShiftType::Ror:
                    if (want) *carry = nx::format("({} >> {}) & 1u", x, n - 1);
                    return nx::format("psv_ror({}, {})", x, n);
                default:  // rrx
                    if (want) *carry = x + " & 1u";
                    return nx::format("((ctx->c << 31) | ({} >> 1))", x);
            }
        }
        case OperandKind::RegShiftReg: {
            const nx::string x = temp("x"), s = temp("s");
            line(nx::format("const uint32_t {} = {};", x, read(in, o.reg)));
            line(nx::format("const uint32_t {} = {};", s, read(in, o.reg2)));
            const u32 type = static_cast<u32>(o.shift);
            if (want) *carry = nx::format("psv_shift_reg_c({}, {}, {}, ctx->c)", x, type, s);
            return nx::format("psv_shift_reg({}, {}, {})", x, type, s);
        }
        default:
            if (want) *carry = "ctx->c";
            return "0u /* operand? */";
    }
}

bool emit_core(Emitter& e, const Instruction& in) {
    return data_processing(e, in) || shift(e, in) || multiply(e, in) || parallel(e, in) || misc(e, in) || branches(e, in) ||
           system(e, in);
}

}  // namespace psvrecore::recomp::detail
