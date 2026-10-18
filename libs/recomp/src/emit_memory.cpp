// Loads and stores of core registers: single, doubleword, multiple, exclusive, and swaps.

#include <bit>

#include "emit.hpp"

namespace psvrecore::recomp::detail {

nx::string Emitter::address(const Instruction& in, const Operand& m, nx::string* writeback) {
    if (writeback) writeback->clear();
    // The PC as a base reads word-aligned (literal loads).
    const nx::string base = m.reg == arm::kPc ? hex(analysis::pc_value(in) & ~3u) : R(m.reg);
    nx::string offset;
    if (m.flags & arm::kMemIndexReg) {
        const nx::string x = read(in, m.reg2);
        switch (m.shift) {
            case arm::ShiftType::Lsl: offset = m.amount ? nx::format("({} << {})", x, m.amount) : x; break;
            case arm::ShiftType::Lsr: offset = m.amount >= 32 ? nx::string("0u") : nx::format("({} >> {})", x, m.amount); break;
            case arm::ShiftType::Asr: offset = nx::format("psv_asr({}, {})", x, m.amount); break;
            case arm::ShiftType::Ror: offset = nx::format("psv_ror({}, {})", x, m.amount); break;
            default: offset = nx::format("((ctx->c << 31) | ({} >> 1))", x); break;
        }
    } else if (m.imm != 0) {
        offset = hex(m.imm);
    }
    const nx::string b = temp("base");
    line(nx::format("const uint32_t {} = {};", b, base));
    nx::string offset_address = b;
    if (!offset.empty()) offset_address = nx::format("{} {} {}", b, (m.flags & arm::kMemSubtract) ? "-" : "+", offset);
    const nx::string a = temp("address");
    line(nx::format("const uint32_t {} = {};", a, (m.flags & arm::kMemPre) ? offset_address : b));
    if ((m.flags & arm::kMemWriteBack) && writeback) {
        if (m.reg == arm::kPc) *writeback = "/* (writeback to the PC: unpredictable) */";
        else *writeback = nx::format("{} = {};", R(m.reg), offset_address);
    }
    return a;
}

namespace {

struct Access {
    Op op;
    u32 size;
    bool load, sign;
};
constexpr Access kSingle[] = {
    {Op::Ldr, 4, true, false},  {Op::Ldrb, 1, true, false}, {Op::Ldrh, 2, true, false},  {Op::Ldrsb, 1, true, true},
    {Op::Ldrsh, 2, true, true}, {Op::Str, 4, false, false}, {Op::Strb, 1, false, false}, {Op::Strh, 2, false, false},
    // Unprivileged forms: the same in user mode.
    {Op::Ldrt, 4, true, false}, {Op::Ldrbt, 1, true, false}, {Op::Ldrht, 2, true, false}, {Op::Ldrsbt, 1, true, true},
    {Op::Ldrsht, 2, true, true}, {Op::Strt, 4, false, false}, {Op::Strbt, 1, false, false}, {Op::Strht, 2, false, false},
};

const char* load_fn(u32 size, bool sign) {
    if (size == 4) return "psv_ld32";
    if (size == 2) return sign ? "psv_lds16" : "psv_ld16";
    return sign ? "psv_lds8" : "psv_ld8";
}
const char* store_fn(u32 size) { return size == 4 ? "psv_st32" : size == 2 ? "psv_st16" : "psv_st8"; }

bool single(Emitter& e, const Instruction& in) {
    const Access* acc = nullptr;
    for (const auto& a : kSingle)
        if (a.op == in.op) acc = &a;
    if (!acc) return false;
    const u32 rt = in.operands[0].reg;
    const Operand& m = in.operands[1];
    // A literal in a read-only segment: its value is known now.
    if (acc->load && m.reg == arm::kPc && !(m.flags & arm::kMemIndexReg) && !(m.flags & arm::kMemWriteBack)) {
        const u32 base = analysis::pc_value(in) & ~3u;
        const u32 at = (m.flags & arm::kMemSubtract) ? base - m.imm : base + m.imm;
        u32 value;
        if (e.constant_load(at, acc->size, value)) {
            if (acc->sign) value = acc->size == 1 ? static_cast<u32>(static_cast<i8>(value)) : static_cast<u32>(static_cast<i16>(value));
            ++e.stats().literals_folded;
            e.write(in, rt, hex(value));
            return true;
        }
    }
    nx::string writeback;
    const nx::string value = acc->load ? nx::string() : e.temp("value");
    if (!acc->load) e.line(nx::format("const uint32_t {} = {};", value, e.read(in, rt)));
    const nx::string a = e.address(in, m, &writeback);
    if (acc->load) {
        const nx::string v = e.temp("v");
        e.line(nx::format("const uint32_t {} = {}({});", v, load_fn(acc->size, acc->sign), a));
        if (!writeback.empty()) e.line(writeback);
        e.write(in, rt, v);
    } else {
        e.line(nx::format("{}({}, {});", store_fn(acc->size), a, value));
        if (!writeback.empty()) e.line(writeback);
    }
    return true;
}

bool dual(Emitter& e, const Instruction& in) {
    if (in.op != Op::Ldrd && in.op != Op::Strd) return false;
    const u32 rt = in.operands[0].reg, rt2 = in.operands[1].reg;
    const Operand& m = in.operands[2];
    nx::string writeback;
    if (in.op == Op::Ldrd) {
        if (m.reg == arm::kPc && !(m.flags & arm::kMemWriteBack)) {
            const u32 base = analysis::pc_value(in) & ~3u;
            const u32 at = (m.flags & arm::kMemSubtract) ? base - m.imm : base + m.imm;
            u32 lo, hi;
            if (e.constant_load(at, 4, lo) && e.constant_load(at + 4, 4, hi)) {
                ++e.stats().literals_folded;
                e.write(in, rt, hex(lo));
                e.write(in, rt2, hex(hi));
                return true;
            }
        }
        const nx::string a = e.address(in, m, &writeback);
        const nx::string lo = e.temp("lo"), hi = e.temp("hi");
        e.line(nx::format("const uint32_t {} = psv_ld32({});", lo, a));
        e.line(nx::format("const uint32_t {} = psv_ld32({} + 4u);", hi, a));
        if (!writeback.empty()) e.line(writeback);
        e.write(in, rt, lo);
        e.write(in, rt2, hi);
    } else {
        const nx::string lo = e.temp("lo"), hi = e.temp("hi");
        e.line(nx::format("const uint32_t {} = {};", lo, e.read(in, rt)));
        e.line(nx::format("const uint32_t {} = {};", hi, e.read(in, rt2)));
        const nx::string a = e.address(in, m, &writeback);
        e.line(nx::format("psv_st32({}, {});", a, lo));
        e.line(nx::format("psv_st32({} + 4u, {});", a, hi));
        if (!writeback.empty()) e.line(writeback);
    }
    return true;
}

bool multiple(Emitter& e, const Instruction& in) {
    bool load, increment, before;
    u32 rn, list;
    bool writeback = (in.flags & arm::kWriteBack) != 0;
    switch (in.op) {
        case Op::Push: load = false, increment = false, before = true, rn = arm::kSp, writeback = true; break;
        case Op::Pop: load = true, increment = true, before = false, rn = arm::kSp, writeback = true; break;
        case Op::Ldm: load = true, increment = true, before = false; break;
        case Op::Ldmib: load = true, increment = true, before = true; break;
        case Op::Ldmda: load = true, increment = false, before = false; break;
        case Op::Ldmdb: load = true, increment = false, before = true; break;
        case Op::Stm: load = false, increment = true, before = false; break;
        case Op::Stmib: load = false, increment = true, before = true; break;
        case Op::Stmda: load = false, increment = false, before = false; break;
        case Op::Stmdb: load = false, increment = false, before = true; break;
        default: return false;
    }
    if (in.op == Op::Push || in.op == Op::Pop) {
        list = in.operands[0].imm;
    } else {
        rn = in.operands[0].reg;
        list = in.operands[1].imm;
    }
    if (in.flags & arm::kUserRegs) {
        e.trap(in, "PSV_TRAP_PRIVILEGED", nx::format("{} ^", arm::mnemonic(in.op)));
        return true;
    }
    const u32 count = static_cast<u32>(std::popcount(list));
    const nx::string base = e.temp("base");
    e.line(nx::format("const uint32_t {} = {};", base, e.read(in, rn)));
    // The lowest address accessed, then one word per register in ascending order.
    nx::string start;
    if (increment) start = before ? nx::format("{} + 4u", base) : base;
    else start = before ? nx::format("{} - {}u", base, 4 * count) : nx::format("{} - {}u", base, 4 * count - 4);
    const nx::string first = e.temp("at");
    e.line(nx::format("const uint32_t {} = {};", first, start));
    const nx::string final_base = increment ? nx::format("{} + {}u", base, 4 * count) : nx::format("{} - {}u", base, 4 * count);
    u32 index = 0;
    if (load) {
        nx::vector<std::pair<u32, nx::string>> loaded;
        for (u32 r = 0; r < 16; ++r) {
            if (!(list & (1u << r))) continue;
            const nx::string v = e.temp("v");
            e.line(nx::format("const uint32_t {} = psv_ld32({} + {}u);", v, first, 4 * index++));
            loaded.emplace_back(r, v);
        }
        if (writeback) e.line(nx::format("{} = {};", R(rn), final_base));
        for (const auto& [r, v] : loaded) e.write(in, r, v);  // (the PC last: a branch)
    } else {
        for (u32 r = 0; r < 16; ++r) {
            if (!(list & (1u << r))) continue;
            // (the base register, when stored, has its original value)
            e.line(nx::format("psv_st32({} + {}u, {});", first, 4 * index++, r == rn ? base : e.read(in, r)));
        }
        if (writeback) e.line(nx::format("{} = {};", R(rn), final_base));
    }
    return true;
}

bool exclusive(Emitter& e, const Instruction& in) {
    nx::string writeback;
    switch (in.op) {
        case Op::Ldrex:
        case Op::Ldrexb:
        case Op::Ldrexh: {
            const u32 size = in.op == Op::Ldrex ? 4 : in.op == Op::Ldrexh ? 2 : 1;
            const nx::string a = e.address(in, in.operands[1], nullptr);
            e.write(in, in.operands[0].reg, nx::format("psv_ldrex(ctx, {}, {})", a, size));
            return true;
        }
        case Op::Ldrexd: {
            const nx::string a = e.address(in, in.operands[2], nullptr);
            const nx::string v = e.temp("v");
            e.line(nx::format("const uint64_t {} = psv_ldrexd(ctx, {});", v, a));
            e.write(in, in.operands[0].reg, nx::format("(uint32_t){}", v));
            e.write(in, in.operands[1].reg, nx::format("(uint32_t)({} >> 32)", v));
            return true;
        }
        case Op::Strex:
        case Op::Strexb:
        case Op::Strexh: {
            const u32 size = in.op == Op::Strex ? 4 : in.op == Op::Strexh ? 2 : 1;
            const nx::string v = e.temp("value");
            e.line(nx::format("const uint32_t {} = {};", v, e.read(in, in.operands[1].reg)));
            const nx::string a = e.address(in, in.operands[2], nullptr);
            e.write(in, in.operands[0].reg, nx::format("psv_strex(ctx, {}, {}, {})", a, v, size));
            return true;
        }
        case Op::Strexd: {
            const nx::string v = e.temp("value");
            e.line(nx::format("const uint64_t {} = (uint64_t){} | ((uint64_t){} << 32);", v, e.read(in, in.operands[1].reg),
                              e.read(in, in.operands[2].reg)));
            const nx::string a = e.address(in, in.operands[3], nullptr);
            e.write(in, in.operands[0].reg, nx::format("psv_strex(ctx, {}, {}, 8)", a, v));
            return true;
        }
        case Op::Swp:
        case Op::Swpb: {
            const nx::string v = e.temp("value");
            e.line(nx::format("const uint32_t {} = {};", v, e.read(in, in.operands[1].reg)));
            const nx::string a = e.address(in, in.operands[2], nullptr);
            e.write(in, in.operands[0].reg, nx::format("psv_swp({}, {}, {})", a, v, in.op == Op::Swp ? 4 : 1));
            return true;
        }
        default: return false;
    }
}

}  // namespace

bool emit_memory(Emitter& e, const Instruction& in) {
    return single(e, in) || dual(e, in) || multiple(e, in) || exclusive(e, in);
}

// A placeholder until the Advanced SIMD translator exists: those instructions trap.
bool emit_simd(Emitter&, const Instruction&) { return false; }

}  // namespace psvrecore::recomp::detail
