#include "psvrecore/interp/interpreter.hpp"

#include <cstring>

#include "exec.hpp"

namespace psvrecore::interp {

using namespace detail;

namespace {

// ConditionPassed (A8.3).
bool condition_passed(const PsvContext& ctx, arm::Cond cond) {
    const bool n = ctx.n, z = ctx.z, c = ctx.c, v = ctx.v;
    switch (cond) {
        case arm::Cond::Eq: return z;
        case arm::Cond::Ne: return !z;
        case arm::Cond::Cs: return c;
        case arm::Cond::Cc: return !c;
        case arm::Cond::Mi: return n;
        case arm::Cond::Pl: return !n;
        case arm::Cond::Vs: return v;
        case arm::Cond::Vc: return !v;
        case arm::Cond::Hi: return c && !z;
        case arm::Cond::Ls: return !c || z;
        case arm::Cond::Ge: return n == v;
        case arm::Cond::Lt: return n != v;
        case arm::Cond::Gt: return !z && n == v;
        case arm::Cond::Le: return z || n != v;
        default: return true;
    }
}

}  // namespace

Result Interpreter::execute(const arm::Instruction& in) {
    trap_kind = 0;
    const u32 next = in.address + in.size;
    if (!in.valid()) {
        trap_kind = PSV_TRAP_UNDEFINED;
        return Result::Trap;
    }
    if (in.flags & arm::kUnpredictable) {
        trap_kind = PSV_TRAP_UNPREDICTABLE;
        return Result::Trap;
    }
    pc = next;
    // (an IT instruction carries its block's first condition; it always executes)
    if (in.op != arm::Op::It && !condition_passed(ctx_, in.cond)) return Result::Next;
    Exec x(*this, ctx_, in);
    if (!exec_core(x) && !exec_memory(x) && !exec_vfp(x) && !exec_simd(x)) {
        trap_kind = PSV_TRAP_UNIMPLEMENTED;
        return Result::Trap;
    }
    if (x.result == Result::Trap) pc = in.address;
    return x.result;
}

Result Interpreter::step() {
    arm::Instruction in;
    const auto mem16 = [&](u32 a) {
        u16 v;
        std::memcpy(&v, psv_membase + (a & address_mask), 2);
        return v;
    };
    if (thumb) {
        const u16 hw1 = mem16(pc);
        const u16 hw2 = arm::thumb_is_32bit(hw1) ? mem16(pc + 2) : u16{0};
        in = arm::decode_thumb(hw1, hw2, pc, it);
    } else {
        u32 w;
        std::memcpy(&w, psv_membase + (pc & address_mask), 4);
        in = arm::decode_arm(w, pc);
    }
    return execute(in);
}

}  // namespace psvrecore::interp
