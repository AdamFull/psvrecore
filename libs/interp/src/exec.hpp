#pragma once

// The interpreter's per-instruction state and the ARM ARM pseudocode functions the instruction groups share.

#include <cstring>

#include "psvrecore/interp/interpreter.hpp"

namespace psvrecore::interp::detail {

using arm::Instruction;
using arm::Op;
using arm::Operand;
using arm::OperandKind;
using arm::ShiftType;

inline i32 sign_extend(u32 value, u32 bits) {
    const u32 m = 1u << (bits - 1);
    return static_cast<i32>(((value & (bits >= 32 ? 0xFFFFFFFFu : (1u << bits) - 1)) ^ m) - m);
}

struct ShiftResult {
    u32 value;
    bool carry;
};

// Shift_C and Shift from the pseudocode (A8.4.3): any amount, including 0 and amounts of 32 or more.
inline ShiftResult shift_c(u32 x, ShiftType type, u32 amount, bool carry_in) {
    if (type == ShiftType::Rrx) return {(carry_in ? 0x80000000u : 0u) | (x >> 1), (x & 1) != 0};
    if (amount == 0) return {x, carry_in};
    switch (type) {
        case ShiftType::Lsl: {
            const u64 extended = amount >= 64 ? 0 : static_cast<u64>(x) << amount;
            return {static_cast<u32>(extended), amount <= 32 && ((extended >> 32) & 1) != 0};
        }
        case ShiftType::Lsr: {
            const u64 extended = x;
            const u32 result = amount >= 32 ? 0u : static_cast<u32>(extended >> amount);
            const bool carry = amount <= 32 && ((extended >> (amount - 1)) & 1) != 0;
            return {result, carry};
        }
        case ShiftType::Asr: {
            const i64 extended = static_cast<i32>(x);
            const u32 n = amount > 63 ? 63 : amount;
            return {static_cast<u32>(extended >> (n > 32 ? 32 : n)), ((extended >> (n - 1)) & 1) != 0};
        }
        default: {  // ROR
            const u32 m = amount % 32;
            const u32 result = m == 0 ? x : (x >> m) | (x << (32 - m));
            return {result, (result >> 31) != 0};
        }
    }
}

struct AddResult {
    u32 value;
    bool carry, overflow;
};

// AddWithCarry (A2.2.1).
inline AddResult add_with_carry(u32 x, u32 y, bool carry_in) {
    const u64 unsigned_sum = static_cast<u64>(x) + static_cast<u64>(y) + (carry_in ? 1u : 0u);
    const i64 signed_sum = static_cast<i64>(static_cast<i32>(x)) + static_cast<i64>(static_cast<i32>(y)) + (carry_in ? 1 : 0);
    const u32 result = static_cast<u32>(unsigned_sum);
    return {result, unsigned_sum != result, signed_sum != static_cast<i32>(result)};
}

class Exec {
public:
    Exec(Interpreter& interp, PsvContext& ctx, const Instruction& in) : it_(interp), ctx(ctx), in(in) {}

    Interpreter& it_;
    PsvContext& ctx;
    const Instruction& in;
    Result result = Result::Next;

    bool thumb() const { return in.mode == arm::Mode::Thumb; }
    u32 pc_read() const { return in.address + (thumb() ? 4 : 8); }
    u32 aligned_pc() const { return pc_read() & ~3u; }

    u32 reg(u32 r) const { return r == arm::kPc ? pc_read() : ctx.r[r]; }

    // Writes to the PC (A2.3.1).
    void branch_write_pc(u32 address) {
        it_.pc = thumb() ? address & ~1u : address & ~3u;
        it_.it = arm::ItState{};
        result = Result::Branched;
    }
    void bx_write_pc(u32 address) {
        if (address & 1) {
            it_.thumb = true;
            it_.pc = address & ~1u;
        } else {
            it_.thumb = false;
            it_.pc = address & ~3u;  // (bit 1 set: unpredictable)
        }
        it_.it = arm::ItState{};
        result = Result::Branched;
    }
    void alu_write_pc(u32 address) {
        if (thumb()) branch_write_pc(address);
        else bx_write_pc(address);
    }
    void load_write_pc(u32 address) { bx_write_pc(address); }

    // A register written by a data-processing instruction (the PC: ALUWritePC).
    void alu_write(u32 r, u32 value) {
        if (r == arm::kPc) alu_write_pc(value);
        else ctx.r[r] = value;
    }
    void load_write(u32 r, u32 value) {
        if (r == arm::kPc) load_write_pc(value);
        else ctx.r[r] = value;
    }

    bool trap(u32 kind) {
        it_.trap_kind = kind;
        result = Result::Trap;
        return true;
    }

    // Guest memory (little-endian, unaligned accesses allowed).
    u8* host(u32 address) const { return psv_membase + (address & it_.address_mask); }
    u32 mem(u32 address, u32 size) const {
        u32 v = 0;
        std::memcpy(&v, host(address), size);
        return v;
    }
    void mem_write(u32 address, u32 size, u32 value) const { std::memcpy(host(address), &value, size); }

    void set_nz(u32 value) {
        ctx.n = value >> 31;
        ctx.z = value == 0;
    }

    // The value of an operand of a data-processing instruction, with the shifter's carry out.
    ShiftResult operand(const Operand& o) const;
    // A memory operand: the address accessed, and the base register's new value (when written back).
    struct Address {
        u32 address;
        bool writeback;
        u32 new_base;
    };
    Address address(const Operand& m) const;
};

bool exec_core(Exec& x);
bool exec_memory(Exec& x);
bool exec_vfp(Exec& x);
bool exec_simd(Exec& x);

}  // namespace psvrecore::interp::detail
