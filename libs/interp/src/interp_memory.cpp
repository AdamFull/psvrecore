// Loads and stores of core registers (A8.8: LDR, STR and kin, LDRD/STRD, LDM/STM, LDREX/STREX, SWP).

#include <bit>

#include "exec.hpp"

namespace psvrecore::interp::detail {

Exec::Address Exec::address(const Operand& m) const {
    // The PC as a base: Align(PC, 4).
    const u32 base = m.reg == arm::kPc ? aligned_pc() : ctx.r[m.reg];
    u32 offset = m.imm;
    if (m.flags & arm::kMemIndexReg) offset = shift_c(reg(m.reg2), m.shift, m.amount, ctx.c != 0).value;
    const bool add = !(m.flags & arm::kMemSubtract), index = (m.flags & arm::kMemPre) != 0;
    const u32 offset_address = add ? base + offset : base - offset;
    return {index ? offset_address : base, (m.flags & arm::kMemWriteBack) != 0, offset_address};
}

namespace {

// The exclusive monitor, in the runtime's model (guest.h): the address, the size and the value loaded.
void set_exclusive(Exec& x, u32 address, u32 size, u64 value) {
    x.ctx.excl_address = address;
    x.ctx.excl_size = size;
    x.ctx.excl_value = value;
}
bool exclusive_passed(Exec& x, u32 address, u32 size) {
    const bool pass = x.ctx.excl_size == size && x.ctx.excl_address == address;
    bool same = false;
    if (pass) {
        u64 current = 0;
        std::memcpy(&current, x.host(address), size);
        const u64 mask = size == 8 ? ~u64{0} : (u64{1} << (8 * size)) - 1;
        same = current == (x.ctx.excl_value & mask);
    }
    x.ctx.excl_size = 0;
    return pass && same;
}

bool single(Exec& x) {
    const auto& in = x.in;
    u32 size;
    bool load, sign = false;
    switch (in.op) {
        case Op::Ldr: case Op::Ldrt: size = 4, load = true; break;
        case Op::Ldrb: case Op::Ldrbt: size = 1, load = true; break;
        case Op::Ldrh: case Op::Ldrht: size = 2, load = true; break;
        case Op::Ldrsb: case Op::Ldrsbt: size = 1, load = true, sign = true; break;
        case Op::Ldrsh: case Op::Ldrsht: size = 2, load = true, sign = true; break;
        case Op::Str: case Op::Strt: size = 4, load = false; break;
        case Op::Strb: case Op::Strbt: size = 1, load = false; break;
        case Op::Strh: case Op::Strht: size = 2, load = false; break;
        default: return false;
    }
    const u32 t = in.operands[0].reg;
    const auto& m = in.operands[1];
    const auto a = x.address(m);
    if (load) {
        u32 data = x.mem(a.address, size);
        if (sign) data = size == 1 ? static_cast<u32>(static_cast<i8>(data)) : static_cast<u32>(static_cast<i16>(data));
        if (a.writeback) x.ctx.r[m.reg] = a.new_base;
        x.load_write(t, data);
    } else {
        x.mem_write(a.address, size, x.reg(t));
        if (a.writeback) x.ctx.r[m.reg] = a.new_base;
    }
    return true;
}

bool dual(Exec& x) {
    const auto& in = x.in;
    if (in.op != Op::Ldrd && in.op != Op::Strd) return false;
    const u32 t = in.operands[0].reg, t2 = in.operands[1].reg;
    const auto& m = in.operands[2];
    const auto a = x.address(m);
    if (in.op == Op::Ldrd) {
        const u32 lo = x.mem(a.address, 4), hi = x.mem(a.address + 4, 4);
        if (a.writeback) x.ctx.r[m.reg] = a.new_base;
        x.ctx.r[t] = lo;
        x.ctx.r[t2] = hi;
    } else {
        x.mem_write(a.address, 4, x.reg(t));
        x.mem_write(a.address + 4, 4, x.reg(t2));
        if (a.writeback) x.ctx.r[m.reg] = a.new_base;
    }
    return true;
}

bool multiple(Exec& x) {
    const auto& in = x.in;
    bool load, increment, before, writeback = (in.flags & arm::kWriteBack) != 0;
    u32 n = arm::kSp, list;
    switch (in.op) {
        case Op::Push: load = false, increment = false, before = true, writeback = true; break;
        case Op::Pop: load = true, increment = true, before = false, writeback = true; break;
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
        n = in.operands[0].reg;
        list = in.operands[1].imm;
    }
    if (in.flags & arm::kUserRegs) return x.trap(PSV_TRAP_PRIVILEGED);
    const u32 count = static_cast<u32>(std::popcount(list));
    const u32 base = x.reg(n);
    // LDMIA/IB/DA/DB: the lowest address is Rn, Rn + 4, Rn - 4 * count + 4, Rn - 4 * count.
    u32 address = increment ? (before ? base + 4 : base) : (before ? base - 4 * count : base - 4 * count + 4);
    const u32 final_base = increment ? base + 4 * count : base - 4 * count;
    if (load) {
        u32 values[16];
        for (u32 r = 0; r < 16; ++r)
            if (list & (1u << r)) values[r] = x.mem(address, 4), address += 4;
        if (writeback) x.ctx.r[n] = final_base;
        for (u32 r = 0; r < 15; ++r)
            if (list & (1u << r)) x.ctx.r[r] = values[r];
        if (list & (1u << arm::kPc)) x.load_write_pc(values[arm::kPc]);
    } else {
        for (u32 r = 0; r < 16; ++r)
            if (list & (1u << r)) {
                x.mem_write(address, 4, r == n ? base : x.reg(r));
                address += 4;
            }
        if (writeback) x.ctx.r[n] = final_base;
    }
    return true;
}

bool exclusive(Exec& x) {
    const auto& in = x.in;
    switch (in.op) {
        case Op::Ldrex:
        case Op::Ldrexb:
        case Op::Ldrexh: {
            const u32 size = in.op == Op::Ldrex ? 4 : in.op == Op::Ldrexh ? 2 : 1;
            const u32 address = x.address(in.operands[1]).address;
            const u32 value = x.mem(address, size);
            set_exclusive(x, address, size, value);
            x.ctx.r[in.operands[0].reg] = value;
            return true;
        }
        case Op::Ldrexd: {
            const u32 address = x.address(in.operands[2]).address;
            const u32 lo = x.mem(address, 4), hi = x.mem(address + 4, 4);
            set_exclusive(x, address, 8, (static_cast<u64>(hi) << 32) | lo);
            x.ctx.r[in.operands[0].reg] = lo;
            x.ctx.r[in.operands[1].reg] = hi;
            return true;
        }
        case Op::Strex:
        case Op::Strexb:
        case Op::Strexh: {
            const u32 size = in.op == Op::Strex ? 4 : in.op == Op::Strexh ? 2 : 1;
            const u32 address = x.address(in.operands[2]).address;
            const u32 value = x.reg(in.operands[1].reg);
            const bool pass = exclusive_passed(x, address, size);
            if (pass) x.mem_write(address, size, value);
            x.ctx.r[in.operands[0].reg] = pass ? 0 : 1;
            return true;
        }
        case Op::Strexd: {
            const u32 address = x.address(in.operands[3]).address;
            const u32 lo = x.reg(in.operands[1].reg), hi = x.reg(in.operands[2].reg);
            const bool pass = exclusive_passed(x, address, 8);
            if (pass) {
                x.mem_write(address, 4, lo);
                x.mem_write(address + 4, 4, hi);
            }
            x.ctx.r[in.operands[0].reg] = pass ? 0 : 1;
            return true;
        }
        case Op::Swp:
        case Op::Swpb: {
            const u32 size = in.op == Op::Swp ? 4 : 1;
            const u32 address = x.address(in.operands[2]).address;
            const u32 value = x.reg(in.operands[1].reg);
            const u32 old = x.mem(address, size);
            x.mem_write(address, size, value);
            x.ctx.r[in.operands[0].reg] = old;
            return true;
        }
        default: return false;
    }
}

}  // namespace

bool exec_memory(Exec& x) { return single(x) || dual(x) || multiple(x) || exclusive(x); }

}  // namespace psvrecore::interp::detail
