// Integer instructions: data processing, multiplies, saturation, parallel arithmetic, extension and bit fields,
// branches, status registers and hints, after the ARM ARM's pseudocode (A8.8).

#include <bit>

#include "exec.hpp"

namespace psvrecore::interp::detail {

ShiftResult Exec::operand(const Operand& o) const {
    const bool c = ctx.c != 0;
    switch (o.kind) {
        case OperandKind::Imm:
        case OperandKind::ModImm:
            // ARMExpandImm_C / ThumbExpandImm_C: the carry is the value's top bit when the encoding rotates.
            return {o.imm, (in.flags & arm::kImmCarry) ? (o.imm >> 31) != 0 : c};
        case OperandKind::Reg: return {reg(o.reg), c};
        case OperandKind::RegShiftImm: return shift_c(reg(o.reg), o.shift, o.amount, c);
        case OperandKind::RegShiftReg: return shift_c(reg(o.reg), o.shift, reg(o.reg2) & 0xFF, c);
        default: return {0, c};
    }
}

namespace {

i32 sint(u32 x) { return static_cast<i32>(x); }
i32 half(u32 x, bool top) { return static_cast<i16>(top ? x >> 16 : x & 0xFFFF); }

// SignedSatQ / UnsignedSatQ.
u32 signed_sat_q(i64 x, u32 n, bool& sat) {
    const i64 hi = (i64{1} << (n - 1)) - 1, lo = -(i64{1} << (n - 1));
    sat = x > hi || x < lo;
    return static_cast<u32>(x > hi ? hi : x < lo ? lo : x) & (n == 32 ? 0xFFFFFFFFu : ((1u << n) - 1));
}
u32 unsigned_sat_q(i64 x, u32 n, bool& sat) {
    const i64 hi = (i64{1} << n) - 1;
    sat = x > hi || x < 0;
    return static_cast<u32>(x > hi ? hi : x < 0 ? 0 : x);
}

bool data_processing(Exec& x) {
    const auto& in = x.in;
    enum Kind { Arith, Logic };
    Kind kind;
    bool test = false;
    switch (in.op) {
        case Op::Add: case Op::Adc: case Op::Sub: case Op::Sbc: case Op::Rsb: case Op::Rsc: kind = Arith; break;
        case Op::Cmp: case Op::Cmn: kind = Arith, test = true; break;
        case Op::And: case Op::Eor: case Op::Orr: case Op::Bic: case Op::Orn: case Op::Mov: case Op::Mvn: kind = Logic; break;
        case Op::Tst: case Op::Teq: kind = Logic, test = true; break;
        default: return false;
    }
    const bool unary = in.op == Op::Mov || in.op == Op::Mvn;
    u32 d = 0, n = 0;
    const Operand* src;
    if (test) n = in.operands[0].reg, src = &in.operands[1];
    else if (unary) d = in.operands[0].reg, src = &in.operands[1];
    else if (in.operand_count == 2) d = n = in.operands[0].reg, src = &in.operands[1];
    else d = in.operands[0].reg, n = in.operands[1].reg, src = &in.operands[2];
    const bool setflags = test || (in.flags & arm::kSetsFlags);
    if (!test && setflags && d == arm::kPc) return x.trap(PSV_TRAP_PRIVILEGED);  // (SUBS PC, LR and kin)
    const ShiftResult shifted = x.operand(*src);
    const u32 rn = unary ? 0 : x.reg(n);
    if (kind == Arith) {
        AddResult r;
        switch (in.op) {
            case Op::Add: case Op::Cmn: r = add_with_carry(rn, shifted.value, false); break;
            case Op::Adc: r = add_with_carry(rn, shifted.value, x.ctx.c != 0); break;
            case Op::Sub: case Op::Cmp: r = add_with_carry(rn, ~shifted.value, true); break;
            case Op::Sbc: r = add_with_carry(rn, ~shifted.value, x.ctx.c != 0); break;
            case Op::Rsb: r = add_with_carry(~rn, shifted.value, true); break;
            default: r = add_with_carry(~rn, shifted.value, x.ctx.c != 0); break;  // rsc
        }
        if (setflags) {
            x.set_nz(r.value);
            x.ctx.c = r.carry;
            x.ctx.v = r.overflow;
        }
        if (!test) x.alu_write(d, r.value);
        return true;
    }
    u32 result;
    switch (in.op) {
        case Op::And: case Op::Tst: result = rn & shifted.value; break;
        case Op::Eor: case Op::Teq: result = rn ^ shifted.value; break;
        case Op::Orr: result = rn | shifted.value; break;
        case Op::Bic: result = rn & ~shifted.value; break;
        case Op::Orn: result = rn | ~shifted.value; break;
        case Op::Mov: result = shifted.value; break;
        default: result = ~shifted.value; break;  // mvn
    }
    if (setflags) {
        x.set_nz(result);
        x.ctx.c = shifted.carry;
    }
    if (!test) x.alu_write(d, result);
    return true;
}

bool shifts(Exec& x) {
    const auto& in = x.in;
    ShiftType type;
    switch (in.op) {
        case Op::Lsl: type = ShiftType::Lsl; break;
        case Op::Lsr: type = ShiftType::Lsr; break;
        case Op::Asr: type = ShiftType::Asr; break;
        case Op::Ror: type = ShiftType::Ror; break;
        case Op::Rrx: type = ShiftType::Rrx; break;
        default: return false;
    }
    const u32 d = in.operands[0].reg;
    ShiftResult r;
    if (type == ShiftType::Rrx) {
        r = shift_c(x.reg(in.operands[1].reg), type, 1, x.ctx.c != 0);
    } else if (in.operand_count == 3 && in.operands[2].kind == OperandKind::Imm) {
        r = shift_c(x.reg(in.operands[1].reg), type, in.operands[2].imm, x.ctx.c != 0);
    } else {
        const u32 m = in.operand_count == 3 ? in.operands[1].reg : in.operands[0].reg;
        const u32 s = in.operand_count == 3 ? in.operands[2].reg : in.operands[1].reg;
        r = shift_c(x.reg(m), type, x.reg(s) & 0xFF, x.ctx.c != 0);
    }
    if (in.flags & arm::kSetsFlags) {
        x.set_nz(r.value);
        x.ctx.c = r.carry;
    }
    x.alu_write(d, r.value);
    return true;
}

bool multiplies(Exec& x) {
    const auto& in = x.in;
    const auto op = [&](int i) { return x.reg(in.operands[i].reg); };
    const auto dst = [&](int i) { return in.operands[i].reg; };
    const bool setflags = in.flags & arm::kSetsFlags;
    const auto write64 = [&](u64 v) {
        x.ctx.r[dst(0)] = static_cast<u32>(v);
        x.ctx.r[dst(1)] = static_cast<u32>(v >> 32);
        if (setflags) {
            x.ctx.n = static_cast<u32>(v >> 63);
            x.ctx.z = v == 0;
        }
    };
    const auto acc64 = [&] { return (static_cast<u64>(op(1)) << 32) | op(0); };
    switch (in.op) {
        case Op::Mul:
        case Op::Mla:
        case Op::Mls: {
            const u64 product = static_cast<u64>(op(1)) * op(2);
            u32 result = static_cast<u32>(product);
            if (in.op == Op::Mla) result += op(3);
            if (in.op == Op::Mls) result = op(3) - result;
            x.ctx.r[dst(0)] = result;
            if (setflags) x.set_nz(result);
            return true;
        }
        case Op::Umull: write64(static_cast<u64>(op(2)) * op(3)); return true;
        case Op::Umlal: write64(static_cast<u64>(op(2)) * op(3) + acc64()); return true;
        case Op::Smull: write64(static_cast<u64>(static_cast<i64>(sint(op(2))) * sint(op(3)))); return true;
        case Op::Smlal: write64(static_cast<u64>(static_cast<i64>(sint(op(2))) * sint(op(3))) + acc64()); return true;
        case Op::Umaal: write64(static_cast<u64>(op(2)) * op(3) + op(0) + op(1)); return true;
        case Op::Smulbb: case Op::Smulbt: case Op::Smultb: case Op::Smultt: {
            const bool nh = in.op == Op::Smultb || in.op == Op::Smultt, mh = in.op == Op::Smulbt || in.op == Op::Smultt;
            x.ctx.r[dst(0)] = static_cast<u32>(half(op(1), nh) * half(op(2), mh));
            return true;
        }
        case Op::Smlabb: case Op::Smlabt: case Op::Smlatb: case Op::Smlatt: {
            const bool nh = in.op == Op::Smlatb || in.op == Op::Smlatt, mh = in.op == Op::Smlabt || in.op == Op::Smlatt;
            const i64 result = static_cast<i64>(half(op(1), nh)) * half(op(2), mh) + sint(op(3));
            x.ctx.r[dst(0)] = static_cast<u32>(result);
            if (result != static_cast<i32>(result)) x.ctx.q = 1;
            return true;
        }
        case Op::Smulwb: case Op::Smulwt: {
            const i64 product = static_cast<i64>(sint(op(1))) * half(op(2), in.op == Op::Smulwt);
            x.ctx.r[dst(0)] = static_cast<u32>(static_cast<u64>(product) >> 16);
            return true;
        }
        case Op::Smlawb: case Op::Smlawt: {
            const i64 result = static_cast<i64>(sint(op(1))) * half(op(2), in.op == Op::Smlawt) + (static_cast<i64>(sint(op(3))) << 16);
            const i64 top = result >> 16;  // result<47:16>
            x.ctx.r[dst(0)] = static_cast<u32>(top);
            if (top != static_cast<i32>(top)) x.ctx.q = 1;
            return true;
        }
        case Op::Smlalbb: case Op::Smlalbt: case Op::Smlaltb: case Op::Smlaltt: {
            const bool nh = in.op == Op::Smlaltb || in.op == Op::Smlaltt, mh = in.op == Op::Smlalbt || in.op == Op::Smlaltt;
            const i64 product = static_cast<i64>(half(op(2), nh)) * half(op(3), mh);
            const u64 result = acc64() + static_cast<u64>(product);
            x.ctx.r[dst(0)] = static_cast<u32>(result);
            x.ctx.r[dst(1)] = static_cast<u32>(result >> 32);
            return true;
        }
        case Op::Smuad: case Op::Smuadx: case Op::Smlad: case Op::Smladx:
        case Op::Smusd: case Op::Smusdx: case Op::Smlsd: case Op::Smlsdx: {
            const bool swap = in.op == Op::Smuadx || in.op == Op::Smladx || in.op == Op::Smusdx || in.op == Op::Smlsdx;
            const bool sub = in.op == Op::Smusd || in.op == Op::Smusdx || in.op == Op::Smlsd || in.op == Op::Smlsdx;
            const bool accumulate = in.op == Op::Smlad || in.op == Op::Smladx || in.op == Op::Smlsd || in.op == Op::Smlsdx;
            u32 m = op(2);
            if (swap) m = (m >> 16) | (m << 16);
            const i64 p1 = static_cast<i64>(half(op(1), false)) * half(m, false);
            const i64 p2 = static_cast<i64>(half(op(1), true)) * half(m, true);
            const i64 result = (sub ? p1 - p2 : p1 + p2) + (accumulate ? sint(op(3)) : 0);
            x.ctx.r[dst(0)] = static_cast<u32>(result);
            if (result != static_cast<i32>(result)) x.ctx.q = 1;
            return true;
        }
        case Op::Smlald: case Op::Smlaldx: case Op::Smlsld: case Op::Smlsldx: {
            const bool swap = in.op == Op::Smlaldx || in.op == Op::Smlsldx, sub = in.op == Op::Smlsld || in.op == Op::Smlsldx;
            u32 m = op(3);
            if (swap) m = (m >> 16) | (m << 16);
            const i64 p1 = static_cast<i64>(half(op(2), false)) * half(m, false);
            const i64 p2 = static_cast<i64>(half(op(2), true)) * half(m, true);
            const u64 result = acc64() + static_cast<u64>(sub ? p1 - p2 : p1 + p2);
            x.ctx.r[dst(0)] = static_cast<u32>(result);
            x.ctx.r[dst(1)] = static_cast<u32>(result >> 32);
            return true;
        }
        case Op::Smmul: case Op::Smmulr: case Op::Smmla: case Op::Smmlar: case Op::Smmls: case Op::Smmlsr: {
            const bool round = in.op == Op::Smmulr || in.op == Op::Smmlar || in.op == Op::Smmlsr;
            // (computed with 128 bits: the 65-bit sums of the pseudocode)
            __int128 result = static_cast<__int128>(sint(op(1))) * sint(op(2));
            if (in.op == Op::Smmla || in.op == Op::Smmlar) result = (static_cast<__int128>(sint(op(3))) << 32) + result;
            if (in.op == Op::Smmls || in.op == Op::Smmlsr) result = (static_cast<__int128>(sint(op(3))) << 32) - result;
            if (round) result += 0x80000000;
            x.ctx.r[dst(0)] = static_cast<u32>(static_cast<u64>(result >> 32));
            return true;
        }
        case Op::Usad8:
        case Op::Usada8: {
            u32 sum = in.op == Op::Usada8 ? op(3) : 0;
            for (int i = 0; i < 4; ++i) {
                const i32 a = (op(1) >> (8 * i)) & 0xFF, b = (op(2) >> (8 * i)) & 0xFF;
                sum += static_cast<u32>(a > b ? a - b : b - a);
            }
            x.ctx.r[dst(0)] = sum;
            return true;
        }
        default: return false;
    }
}

bool saturating(Exec& x) {
    const auto& in = x.in;
    const auto op = [&](int i) { return x.reg(in.operands[i].reg); };
    const u32 d = in.operands[0].reg;
    bool sat = false, sat2 = false;
    switch (in.op) {
        case Op::Qadd: x.ctx.r[d] = signed_sat_q(static_cast<i64>(sint(op(1))) + sint(op(2)), 32, sat); break;
        case Op::Qsub: x.ctx.r[d] = signed_sat_q(static_cast<i64>(sint(op(1))) - sint(op(2)), 32, sat); break;
        case Op::Qdadd: {
            const u32 doubled = signed_sat_q(2 * static_cast<i64>(sint(op(2))), 32, sat);
            x.ctx.r[d] = signed_sat_q(static_cast<i64>(sint(op(1))) + sint(doubled), 32, sat2);
            break;
        }
        case Op::Qdsub: {
            const u32 doubled = signed_sat_q(2 * static_cast<i64>(sint(op(2))), 32, sat);
            x.ctx.r[d] = signed_sat_q(static_cast<i64>(sint(op(1))) - sint(doubled), 32, sat2);
            break;
        }
        case Op::Ssat:
        case Op::Usat: {
            const i64 operand = sint(x.operand(in.operands[2]).value);
            const u32 n = in.operands[1].imm;
            x.ctx.r[d] = in.op == Op::Ssat ? signed_sat_q(operand, n, sat) : unsigned_sat_q(operand, n, sat);
            if (in.op == Op::Ssat && n < 32) x.ctx.r[d] = static_cast<u32>(sign_extend(x.ctx.r[d], n));
            break;
        }
        case Op::Ssat16:
        case Op::Usat16: {
            const u32 m = op(2), n = in.operands[1].imm;
            u32 lo, hi;
            if (in.op == Op::Ssat16) {
                lo = signed_sat_q(half(m, false), n, sat);
                hi = signed_sat_q(half(m, true), n, sat2);
                lo = static_cast<u32>(sign_extend(lo, n)) & 0xFFFF;
                hi = static_cast<u32>(sign_extend(hi, n)) & 0xFFFF;
            } else {
                lo = unsigned_sat_q(half(m, false), n, sat);
                hi = unsigned_sat_q(half(m, true), n, sat2);
            }
            x.ctx.r[d] = lo | (hi << 16);
            break;
        }
        default: return false;
    }
    if (sat || sat2) x.ctx.q = 1;
    return true;
}

// Parallel addition and subtraction (A8.8.x, the ADD16/ASX/SAX/SUB16/ADD8/SUB8 families).
bool parallel(Exec& x) {
    const auto& in = x.in;
    struct P {
        Op op;
        bool is_signed;
        int mode;  // 0 modular (GE), 1 saturating, 2 halving
        int form;  // 0 add16, 1 asx, 2 sax, 3 sub16, 4 add8, 5 sub8
    };
    static constexpr P kOps[] = {
        {Op::Sadd16, true, 0, 0}, {Op::Sasx, true, 0, 1}, {Op::Ssax, true, 0, 2}, {Op::Ssub16, true, 0, 3}, {Op::Sadd8, true, 0, 4},
        {Op::Ssub8, true, 0, 5}, {Op::Qadd16, true, 1, 0}, {Op::Qasx, true, 1, 1}, {Op::Qsax, true, 1, 2}, {Op::Qsub16, true, 1, 3},
        {Op::Qadd8, true, 1, 4}, {Op::Qsub8, true, 1, 5}, {Op::Shadd16, true, 2, 0}, {Op::Shasx, true, 2, 1}, {Op::Shsax, true, 2, 2},
        {Op::Shsub16, true, 2, 3}, {Op::Shadd8, true, 2, 4}, {Op::Shsub8, true, 2, 5}, {Op::Uadd16, false, 0, 0}, {Op::Uasx, false, 0, 1},
        {Op::Usax, false, 0, 2}, {Op::Usub16, false, 0, 3}, {Op::Uadd8, false, 0, 4}, {Op::Usub8, false, 0, 5}, {Op::Uqadd16, false, 1, 0},
        {Op::Uqasx, false, 1, 1}, {Op::Uqsax, false, 1, 2}, {Op::Uqsub16, false, 1, 3}, {Op::Uqadd8, false, 1, 4}, {Op::Uqsub8, false, 1, 5},
        {Op::Uhadd16, false, 2, 0}, {Op::Uhasx, false, 2, 1}, {Op::Uhsax, false, 2, 2}, {Op::Uhsub16, false, 2, 3}, {Op::Uhadd8, false, 2, 4},
        {Op::Uhsub8, false, 2, 5},
    };
    const P* p = nullptr;
    for (const auto& e : kOps)
        if (e.op == in.op) p = &e;
    if (!p) return false;
    const u32 n = x.reg(in.operands[1].reg), m = x.reg(in.operands[2].reg);
    const bool bytes = p->form >= 4;
    const u32 width = bytes ? 8 : 16, lanes = bytes ? 4 : 2, mask = (1u << width) - 1;
    const auto element = [&](u32 v, u32 i) -> i64 {
        const u32 e = (v >> (i * width)) & mask;
        if (!p->is_signed) return e;
        return bytes ? static_cast<i8>(e) : static_cast<i16>(e);
    };
    u32 result = 0, ge = 0;
    for (u32 i = 0; i < lanes; ++i) {
        i64 value;
        switch (p->form) {
            case 1: value = i == 0 ? element(n, 0) - element(m, 1) : element(n, 1) + element(m, 0); break;  // asx
            case 2: value = i == 0 ? element(n, 0) + element(m, 1) : element(n, 1) - element(m, 0); break;  // sax
            case 3: case 5: value = element(n, i) - element(m, i); break;
            default: value = element(n, i) + element(m, i); break;
        }
        const bool is_sum = p->form == 0 || p->form == 4 || (p->form == 1 && i == 1) || (p->form == 2 && i == 0);
        u32 lane;
        if (p->mode == 0) {
            lane = static_cast<u32>(value) & mask;
            bool ge_bit;
            if (p->is_signed) ge_bit = value >= 0;
            else ge_bit = is_sum ? value >= (i64{1} << width) : value >= 0;
            if (ge_bit) ge |= (bytes ? 1u : 3u) << (bytes ? i : 2 * i);
        } else if (p->mode == 1) {
            bool sat;
            lane = (p->is_signed ? signed_sat_q(value, width, sat) : unsigned_sat_q(value, width, sat)) & mask;
        } else {
            lane = static_cast<u32>(value >> 1) & mask;
        }
        result |= lane << (i * width);
    }
    if (p->mode == 0) x.ctx.ge = ge;
    x.ctx.r[in.operands[0].reg] = result;
    return true;
}

u32 ror(u32 v, u32 n) { return n % 32 == 0 ? v : (v >> (n % 32)) | (v << (32 - n % 32)); }

bool misc(Exec& x) {
    const auto& in = x.in;
    const auto op = [&](int i) { return x.reg(in.operands[i].reg); };
    const auto rotated = [&](int i) {
        const auto& o = in.operands[i];
        return o.kind == OperandKind::RegShiftImm ? ror(x.reg(o.reg), o.amount) : x.reg(o.reg);
    };
    const u32 d = in.operands[0].reg;
    auto& r = x.ctx.r;
    switch (in.op) {
        case Op::Movw: x.alu_write(d, in.operands[1].imm); return true;
        case Op::Movt: r[d] = (r[d] & 0xFFFF) | (in.operands[1].imm << 16); return true;
        case Op::Adr: x.alu_write(d, x.aligned_pc() + in.operands[1].imm); return true;
        case Op::Clz: r[d] = static_cast<u32>(std::countl_zero(op(1))); return true;
        case Op::Rbit: {
            u32 v = op(1), out = 0;
            for (int i = 0; i < 32; ++i) out |= ((v >> i) & 1u) << (31 - i);
            r[d] = out;
            return true;
        }
        case Op::Rev: {
            const u32 v = op(1);
            r[d] = (v << 24) | ((v << 8) & 0xFF0000) | ((v >> 8) & 0xFF00) | (v >> 24);
            return true;
        }
        case Op::Rev16: {
            const u32 v = op(1);
            r[d] = ((v & 0xFF) << 8) | ((v >> 8) & 0xFF) | ((v & 0xFF0000) << 8) | ((v >> 8) & 0xFF0000);
            return true;
        }
        case Op::Revsh: {
            const u32 v = op(1);
            r[d] = static_cast<u32>(static_cast<i32>(static_cast<i8>(v & 0xFF)) * 256) | ((v >> 8) & 0xFF);
            return true;
        }
        case Op::Sel: {
            const u32 n = op(1), m = op(2);
            u32 out = 0;
            for (int i = 0; i < 4; ++i) out |= (((x.ctx.ge >> i) & 1) ? n : m) & (0xFFu << (8 * i));
            r[d] = out;
            return true;
        }
        case Op::Sxtb: r[d] = static_cast<u32>(static_cast<i8>(rotated(1))); return true;
        case Op::Sxth: r[d] = static_cast<u32>(static_cast<i16>(rotated(1))); return true;
        case Op::Uxtb: r[d] = rotated(1) & 0xFF; return true;
        case Op::Uxth: r[d] = rotated(1) & 0xFFFF; return true;
        case Op::Sxtab: r[d] = op(1) + static_cast<u32>(static_cast<i8>(rotated(2))); return true;
        case Op::Sxtah: r[d] = op(1) + static_cast<u32>(static_cast<i16>(rotated(2))); return true;
        case Op::Uxtab: r[d] = op(1) + (rotated(2) & 0xFF); return true;
        case Op::Uxtah: r[d] = op(1) + (rotated(2) & 0xFFFF); return true;
        case Op::Sxtb16: case Op::Uxtb16: case Op::Sxtab16: case Op::Uxtab16: {
            const bool add = in.op == Op::Sxtab16 || in.op == Op::Uxtab16, sign = in.op == Op::Sxtb16 || in.op == Op::Sxtab16;
            const u32 v = rotated(add ? 2 : 1), n = add ? op(1) : 0;
            const auto ext = [&](u32 byte) { return sign ? static_cast<u32>(static_cast<i8>(byte)) : byte & 0xFF; };
            const u32 lo = ((n & 0xFFFF) + ext(v & 0xFF)) & 0xFFFF;
            const u32 hi = ((n >> 16) + ext((v >> 16) & 0xFF)) & 0xFFFF;
            r[d] = lo | (hi << 16);
            return true;
        }
        case Op::Pkhbt: {
            const u32 m = x.operand(in.operands[2]).value;
            r[d] = (op(1) & 0xFFFF) | (m & 0xFFFF0000);
            return true;
        }
        case Op::Pkhtb: {
            const u32 m = x.operand(in.operands[2]).value;
            r[d] = (op(1) & 0xFFFF0000) | (m & 0xFFFF);
            return true;
        }
        case Op::Bfc:
        case Op::Bfi: {
            const bool insert = in.op == Op::Bfi;
            const u32 lsb = in.operands[insert ? 2 : 1].imm, width = in.operands[insert ? 3 : 2].imm;
            const u32 source = insert ? op(1) : 0;
            u32 v = r[d];
            for (u32 i = 0; i < width; ++i) {
                const u32 bit = (source >> i) & 1;
                v = (v & ~(1u << (lsb + i))) | (bit << (lsb + i));
            }
            r[d] = v;
            return true;
        }
        case Op::Sbfx:
        case Op::Ubfx: {
            const u32 lsb = in.operands[2].imm, width = in.operands[3].imm;
            if (lsb + width > 32) return x.trap(PSV_TRAP_UNPREDICTABLE);
            const u32 field = width == 32 ? op(1) >> lsb : (op(1) >> lsb) & ((1u << width) - 1);
            r[d] = in.op == Op::Ubfx ? field : static_cast<u32>(sign_extend(field, width));
            return true;
        }
        default: return false;
    }
}

bool branches(Exec& x) {
    const auto& in = x.in;
    const u32 next = in.address + in.size;
    switch (in.op) {
        case Op::B: x.branch_write_pc(in.operands[0].imm); return true;
        case Op::Cbz:
        case Op::Cbnz:
            if ((x.ctx.r[in.operands[0].reg] == 0) == (in.op == Op::Cbz)) x.branch_write_pc(in.operands[1].imm);
            return true;
        case Op::Bl:
            x.ctx.r[arm::kLr] = x.thumb() ? next | 1 : next;
            x.branch_write_pc(in.operands[0].imm);
            return true;
        case Op::Blx:
            if (in.operands[0].kind == OperandKind::Label) {  // to the other instruction set
                x.ctx.r[arm::kLr] = x.thumb() ? next | 1 : next;
                x.bx_write_pc(x.thumb() ? in.operands[0].imm : in.operands[0].imm | 1);
            } else {
                const u32 target = x.reg(in.operands[0].reg);
                x.ctx.r[arm::kLr] = x.thumb() ? next | 1 : next;
                x.bx_write_pc(target);
            }
            return true;
        case Op::Bx:
        case Op::Bxj:
            x.bx_write_pc(x.reg(in.operands[0].reg));
            return true;
        case Op::Tbb:
        case Op::Tbh: {
            const auto& m = in.operands[0];
            const u32 base = x.reg(m.reg), index = x.reg(m.reg2);
            const u32 halfwords = in.op == Op::Tbb ? x.mem(base + index, 1) : x.mem(base + 2 * index, 2);
            x.branch_write_pc(x.pc_read() + 2 * halfwords);
            return true;
        }
        default: return false;
    }
}

bool status_and_hints(Exec& x) {
    const auto& in = x.in;
    switch (in.op) {
        case Op::It: case Op::Nop: case Op::Hint: case Op::Dbg: case Op::Sev: case Op::Isb: case Op::Pld: case Op::Pldw:
        case Op::Pli: case Op::Yield: case Op::Wfe: case Op::Wfi: case Op::Dmb: case Op::Dsb:
            return true;
        case Op::Clrex: x.ctx.excl_size = 0; return true;
        case Op::Mrs:
            if (in.operands[1].imm != 1) return x.trap(PSV_TRAP_PRIVILEGED);
            x.ctx.r[in.operands[0].reg] = (x.ctx.n << 31) | (x.ctx.z << 30) | (x.ctx.c << 29) | (x.ctx.v << 28) | (x.ctx.q << 27) |
                                          (x.ctx.ge << 16) | 0x10;
            return true;
        case Op::Msr: {
            const u32 code = in.operands[0].imm;
            if (code & 0x10) return x.trap(PSV_TRAP_PRIVILEGED);
            const auto& src = in.operands[1];
            const u32 value = src.kind == OperandKind::Reg ? x.reg(src.reg) : src.imm;
            if (code & 8) {
                x.ctx.n = value >> 31;
                x.ctx.z = (value >> 30) & 1;
                x.ctx.c = (value >> 29) & 1;
                x.ctx.v = (value >> 28) & 1;
                x.ctx.q = (value >> 27) & 1;
            }
            if (code & 4) x.ctx.ge = (value >> 16) & 0xF;
            return true;
        }
        case Op::Mrc:
        case Op::Mcr: {
            const u32 cp = in.operands[0].imm, opc1 = in.operands[1].imm, crn = in.operands[3].reg, crm = in.operands[4].reg,
                      opc2 = in.operands[5].imm;
            if (cp == 15 && opc1 == 0 && crn == 13 && crm == 0 && in.operands[2].kind == OperandKind::Reg) {
                const u32 rt = in.operands[2].reg;
                if (opc2 == 2) {
                    if (in.op == Op::Mrc) x.ctx.r[rt] = x.ctx.tpidrurw;
                    else x.ctx.tpidrurw = x.reg(rt);
                    return true;
                }
                if (opc2 == 3 && in.op == Op::Mrc) {
                    x.ctx.r[rt] = x.ctx.tpidruro;
                    return true;
                }
            }
            return x.trap(PSV_TRAP_PRIVILEGED);
        }
        case Op::Setend:
            if (in.operands[0].imm == 2) return true;
            return x.trap(PSV_TRAP_BIG_ENDIAN);
        case Op::Svc: return x.trap(PSV_TRAP_PRIVILEGED);
        case Op::Bkpt: return x.trap(PSV_TRAP_BKPT);
        case Op::Udf: return x.trap(PSV_TRAP_UDF);
        default: return false;
    }
}

}  // namespace

bool exec_core(Exec& x) {
    return data_processing(x) || shifts(x) || multiplies(x) || saturating(x) || parallel(x) || misc(x) || branches(x) ||
           status_and_hints(x);
}

}  // namespace psvrecore::interp::detail
