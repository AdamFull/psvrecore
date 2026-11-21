#include "psvrecore/analysis/registers.hpp"

#include <algorithm>
#include <bit>

#include "psvrecore/analysis/flow.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/hash_set.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::analysis {

using arm::Op;
using arm::OperandKind;

nx::string register_names(RegSet r) {
    if (!r) return "none";
    static constexpr const char* kNames[] = {"r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "r12", "sp", "lr"};
    nx::string s;
    for (u32 k = 0; k < 15;) {
        if (!(r & (1u << k))) {
            ++k;
            continue;
        }
        u32 end = k;
        while (end + 1 < 13 && (r & (1u << (end + 1)))) ++end;  // (runs of r0-r12)
        if (!s.empty()) s += ' ';
        s += kNames[k];
        if (end > k) s += nx::format("-{}", kNames[end]);
        k = end + 1;
    }
    return s;
}

const char* entry_reason_name(EntryReason r) {
    switch (r) {
        case EntryReason::None: return "none";
        case EntryReason::Import: return "calls an import";
        case EntryReason::Indirect: return "calls through the runtime";
        case EntryReason::Svc: return "svc";
        case EntryReason::Trap: return "may trap";
        case EntryReason::PublicCallee: return "calls a function without an internal entry";
        case EntryReason::ViaCallee: return "calls a function that reaches outside";
        case EntryReason::Disabled: return "internal calls off";
        case EntryReason::Excluded: return "excluded (--no-inline)";
        case EntryReason::NoCode: return "no code";
        case EntryReason::NoReturn: return "never returns";
        case EntryReason::NoCallers: return "no direct callers";
        case EntryReason::OtherReturn: return "may return elsewhere than its LR";
        case EntryReason::TailCall: return "tail call";
    }
    return "?";
}

namespace {

constexpr RegSet bit(u32 r) { return r < 15 ? static_cast<RegSet>(1u << r) : RegSet(0); }
constexpr RegSet mask(u32 list) { return static_cast<RegSet>(list & kAllRegisters); }

// The registers an operand reads as a source (a register, a shifted register, a memory operand's base and index).
RegSet operand_uses(const arm::Operand& o) {
    switch (o.kind) {
        case OperandKind::Reg:
        case OperandKind::RegShiftImm: return bit(o.reg);
        case OperandKind::RegShiftReg: return static_cast<RegSet>(bit(o.reg) | bit(o.reg2));
        case OperandKind::Mem: {
            RegSet r = bit(o.reg);
            if (o.flags & (arm::kMemIndexReg | arm::kMemPostReg)) r |= bit(o.reg2);
            return r;
        }
        default: return 0;
    }
}
// A memory operand's base when the instruction writes it back.
RegSet mem_writeback(const arm::Operand& m) {
    return (m.flags & (arm::kMemWriteBack | arm::kMemPostReg)) ? bit(m.reg) : RegSet(0);
}

}  // namespace

RegisterEffect register_effect(const arm::Instruction& in) {
    RegisterEffect e;
    const auto& o = in.operands;
    const u8 n = in.operand_count;
    const auto src = [&](int i) { return i < n ? operand_uses(o[i]) : RegSet(0); };
    const auto dst = [&](int i) { return i < n && o[i].kind == OperandKind::Reg ? bit(o[i].reg) : RegSet(0); };
    if (!in.valid() || (in.flags & arm::kUnpredictable)) {
        e.traps = true;
        return e;
    }
    switch (in.op) {
        // Data processing: {rd, rn, op2}, {rdn, op2}, {rn, op2} (compares), {rd, op2} (mov, mvn).
        case Op::Add: case Op::Adc: case Op::Sub: case Op::Sbc: case Op::Rsb: case Op::Rsc:
        case Op::And: case Op::Eor: case Op::Orr: case Op::Bic: case Op::Orn: {
            if (o[0].reg == arm::kPc && (in.flags & arm::kSetsFlags)) {  // (an exception return: a trap)
                e.traps = true;
                return e;
            }
            e.uses = n == 2 ? static_cast<RegSet>(src(0) | src(1)) : static_cast<RegSet>(src(1) | src(2));
            e.defs = dst(0);
            return e;
        }
        case Op::Mov: case Op::Mvn:
            if (o[0].reg == arm::kPc && (in.flags & arm::kSetsFlags)) {
                e.traps = true;
                return e;
            }
            e.uses = src(1);
            e.defs = dst(0);
            return e;
        case Op::Cmp: case Op::Cmn: case Op::Tst: case Op::Teq: e.uses = static_cast<RegSet>(src(0) | src(1)); return e;
        case Op::Lsl: case Op::Lsr: case Op::Asr: case Op::Ror: case Op::Rrx:
            e.uses = static_cast<RegSet>(src(n == 3 ? 1 : 0) | src(n == 3 ? 2 : 1));
            if (in.op == Op::Rrx || (n == 3 && o[2].kind == OperandKind::Imm)) e.uses = src(1);
            e.defs = dst(0);
            return e;
        // Multiplies.
        case Op::Mul: e.uses = static_cast<RegSet>(src(1) | src(2)), e.defs = dst(0); return e;
        case Op::Mla: case Op::Mls: e.uses = static_cast<RegSet>(src(1) | src(2) | src(3)), e.defs = dst(0); return e;
        case Op::Umull: case Op::Smull: e.uses = static_cast<RegSet>(src(2) | src(3)), e.defs = static_cast<RegSet>(dst(0) | dst(1)); return e;
        case Op::Umlal: case Op::Smlal: case Op::Umaal:
        case Op::Smlalbb: case Op::Smlalbt: case Op::Smlaltb: case Op::Smlaltt:
        case Op::Smlald: case Op::Smlaldx: case Op::Smlsld: case Op::Smlsldx:
            e.uses = static_cast<RegSet>(src(0) | src(1) | src(2) | src(3));
            e.defs = static_cast<RegSet>(dst(0) | dst(1));
            return e;
        case Op::Smulbb: case Op::Smulbt: case Op::Smultb: case Op::Smultt: case Op::Smulwb: case Op::Smulwt:
        case Op::Smuad: case Op::Smuadx: case Op::Smusd: case Op::Smusdx: case Op::Smmul: case Op::Smmulr: case Op::Usad8:
            e.uses = static_cast<RegSet>(src(1) | src(2)), e.defs = dst(0);
            return e;
        case Op::Smlabb: case Op::Smlabt: case Op::Smlatb: case Op::Smlatt: case Op::Smlawb: case Op::Smlawt:
        case Op::Smlad: case Op::Smladx: case Op::Smlsd: case Op::Smlsdx: case Op::Smmla: case Op::Smmlar: case Op::Smmls:
        case Op::Smmlsr: case Op::Usada8:
            e.uses = static_cast<RegSet>(src(1) | src(2) | src(3)), e.defs = dst(0);
            return e;
        // Parallel arithmetic, saturation, packing, extension.
        case Op::Sadd16: case Op::Sasx: case Op::Ssax: case Op::Ssub16: case Op::Sadd8: case Op::Ssub8:
        case Op::Qadd16: case Op::Qasx: case Op::Qsax: case Op::Qsub16: case Op::Qadd8: case Op::Qsub8:
        case Op::Shadd16: case Op::Shasx: case Op::Shsax: case Op::Shsub16: case Op::Shadd8: case Op::Shsub8:
        case Op::Uadd16: case Op::Uasx: case Op::Usax: case Op::Usub16: case Op::Uadd8: case Op::Usub8:
        case Op::Uqadd16: case Op::Uqasx: case Op::Uqsax: case Op::Uqsub16: case Op::Uqadd8: case Op::Uqsub8:
        case Op::Uhadd16: case Op::Uhasx: case Op::Uhsax: case Op::Uhsub16: case Op::Uhadd8: case Op::Uhsub8:
        case Op::Sel: case Op::Qadd: case Op::Qsub: case Op::Qdadd: case Op::Qdsub:
        case Op::Sxtab: case Op::Sxtah: case Op::Uxtab: case Op::Uxtah: case Op::Sxtab16: case Op::Uxtab16:
        case Op::Pkhbt: case Op::Pkhtb: case Op::Bfi:
            e.uses = static_cast<RegSet>(src(1) | src(2)), e.defs = dst(0);
            if (in.op == Op::Bfi) e.uses |= src(0);
            return e;
        case Op::Ssat: case Op::Usat: case Op::Ssat16: case Op::Usat16: e.uses = src(2), e.defs = dst(0); return e;
        case Op::Clz: case Op::Rbit: case Op::Rev: case Op::Rev16: case Op::Revsh:
        case Op::Sxtb: case Op::Sxth: case Op::Uxtb: case Op::Uxth: case Op::Sxtb16: case Op::Uxtb16:
            e.uses = src(1), e.defs = dst(0);
            return e;
        case Op::Sbfx: case Op::Ubfx:
            if (o[2].imm + o[3].imm > 32) {  // (not translated: a trap)
                e.traps = true;
                return e;
            }
            e.uses = src(1), e.defs = dst(0);
            return e;
        case Op::Bfc: e.uses = src(0), e.defs = dst(0); return e;
        case Op::Movw: case Op::Adr: e.defs = dst(0); return e;
        case Op::Movt: e.uses = src(0), e.defs = dst(0); return e;
        // Branches and calls.
        case Op::B: return e;
        case Op::Cbz: case Op::Cbnz: e.uses = src(0); return e;
        case Op::Bl: e.defs = kLrRegister; return e;
        case Op::Blx:
            e.defs = kLrRegister;
            if (o[0].kind != OperandKind::Label) e.uses = src(0);
            return e;
        case Op::Bx: case Op::Bxj: e.uses = src(0); return e;
        case Op::Tbb: case Op::Tbh: e.uses = src(0), e.traps = true; return e;  // (an index outside its table)
        // System.
        case Op::Svc: return e;  // (a call: what is dirty goes to the context before it)
        case Op::Bkpt: case Op::Udf: e.traps = true; return e;
        case Op::Setend:
            if (o[0].imm != 2) e.traps = true;
            return e;
        case Op::Mrs:
            if (o[1].imm != 1) e.traps = true;
            else e.defs = dst(0);
            return e;
        case Op::Msr:
            if (o[0].imm & 0x10) e.traps = true;
            else e.uses = src(1);
            return e;
        case Op::Mrc: case Op::Mcr: {
            const u32 cp = o[0].imm, opc1 = o[1].imm, crn = o[3].reg, crm = o[4].reg, opc2 = o[5].imm;
            if (cp == 15 && opc1 == 0 && crn == 13 && crm == 0 && (opc2 == 2 || opc2 == 3) && o[2].kind == OperandKind::Reg) {
                if (in.op == Op::Mrc) {
                    e.defs = dst(2);
                    return e;
                }
                if (opc2 == 2) {
                    e.uses = src(2);
                    return e;
                }
            }
            e.traps = true;
            return e;
        }
        case Op::Cps: case Op::Cpsie: case Op::Cpsid: case Op::Smc: case Op::Subs: case Op::Eret:
        case Op::Srsda: case Op::Srsdb: case Op::Srsia: case Op::Srsib: case Op::Rfeda: case Op::Rfedb: case Op::Rfeia:
        case Op::Rfeib: case Op::Cdp: case Op::Cdp2: case Op::Mcr2: case Op::Mrc2: case Op::Mcrr: case Op::Mcrr2:
        case Op::Mrrc: case Op::Mrrc2: case Op::Ldc: case Op::Ldcl: case Op::Ldc2: case Op::Ldc2l: case Op::Stc: case Op::Stcl:
        case Op::Stc2: case Op::Stc2l:
            e.traps = true;
            return e;
        // Loads and stores.
        case Op::Ldr: case Op::Ldrb: case Op::Ldrh: case Op::Ldrsb: case Op::Ldrsh:
        case Op::Ldrt: case Op::Ldrbt: case Op::Ldrht: case Op::Ldrsbt: case Op::Ldrsht:
            e.uses = src(1);
            e.defs = static_cast<RegSet>(dst(0) | (o[1].reg == arm::kPc ? RegSet(0) : mem_writeback(o[1])));
            return e;
        case Op::Str: case Op::Strb: case Op::Strh: case Op::Strt: case Op::Strbt: case Op::Strht:
            e.uses = static_cast<RegSet>(src(0) | src(1));
            e.defs = o[1].reg == arm::kPc ? RegSet(0) : mem_writeback(o[1]);
            return e;
        case Op::Ldrd:
            e.uses = src(2);
            e.defs = static_cast<RegSet>(dst(0) | dst(1) | (o[2].reg == arm::kPc ? RegSet(0) : mem_writeback(o[2])));
            return e;
        case Op::Strd:
            e.uses = static_cast<RegSet>(src(0) | src(1) | src(2));
            e.defs = o[2].reg == arm::kPc ? RegSet(0) : mem_writeback(o[2]);
            return e;
        case Op::Push: case Op::Pop: case Op::Ldm: case Op::Ldmib: case Op::Ldmda: case Op::Ldmdb:
        case Op::Stm: case Op::Stmib: case Op::Stmda: case Op::Stmdb: {
            if (in.flags & arm::kUserRegs) {
                e.traps = true;
                return e;
            }
            const bool pp = in.op == Op::Push || in.op == Op::Pop;
            const u32 rn = pp ? arm::kSp : o[0].reg;
            const u32 list = pp ? o[0].imm : o[1].imm;
            const bool load = in.op == Op::Pop || in.op == Op::Ldm || in.op == Op::Ldmib || in.op == Op::Ldmda || in.op == Op::Ldmdb;
            const bool writeback = pp || (in.flags & arm::kWriteBack);
            e.uses = bit(rn);
            if (load) e.defs = mask(list);
            else e.uses |= mask(list);
            if (writeback) e.defs |= bit(rn);
            return e;
        }
        case Op::Ldrex: case Op::Ldrexb: case Op::Ldrexh: e.uses = src(1), e.defs = dst(0); return e;
        case Op::Ldrexd: e.uses = src(2), e.defs = static_cast<RegSet>(dst(0) | dst(1)); return e;
        case Op::Strex: case Op::Strexb: case Op::Strexh: case Op::Swp: case Op::Swpb:
            e.uses = static_cast<RegSet>(src(1) | src(2)), e.defs = dst(0);
            return e;
        case Op::Strexd: e.uses = static_cast<RegSet>(src(1) | src(2) | src(3)), e.defs = dst(0); return e;
        case Op::Pld: case Op::Pldw: case Op::Pli: e.uses = src(0); return e;  // (no access: the reads are a superset)
        // Floating point and Advanced SIMD: their core registers.
        case Op::Vmov: {
            if (n == 2 && o[0].kind == OperandKind::Reg) e.defs = dst(0);  // vmov rt, sn / rt, dn[x]
            else if (n == 2 && o[1].kind == OperandKind::Reg) e.uses = src(1);  // vmov sn, rt / dn[x], rt
            else if (n == 4 && o[0].kind == OperandKind::Reg) e.defs = static_cast<RegSet>(dst(0) | dst(1));
            else if (n == 4) e.uses = static_cast<RegSet>(src(2) | src(3));
            else if (n == 3 && o[0].kind == OperandKind::Reg) e.defs = static_cast<RegSet>(dst(0) | dst(1));
            else if (n == 3 && o[1].kind == OperandKind::Reg) e.uses = static_cast<RegSet>(src(1) | src(2));
            return e;
        }
        case Op::Vldr: case Op::Vstr: e.uses = o[1].reg == arm::kPc ? RegSet(0) : src(1); return e;
        case Op::Vldmia: case Op::Vldmdb: case Op::Vstmia: case Op::Vstmdb: case Op::Vpush: case Op::Vpop: {
            const bool pp = in.op == Op::Vpush || in.op == Op::Vpop;
            const u32 rn = pp ? arm::kSp : o[0].reg;
            e.uses = bit(rn);
            if (pp || (in.flags & arm::kWriteBack)) e.defs = bit(rn);
            return e;
        }
        case Op::Vmrs:
            if (o[1].imm != 5) e.traps = true;
            else if (o[0].kind == OperandKind::Reg) e.defs = dst(0);
            return e;
        case Op::Vmsr:
            e.traps = true;  // (an FPSCR setting not implemented, or another system register)
            if (o[0].imm == 5) e.uses = src(1);
            return e;
        case Op::Vdup: e.uses = o[1].kind == OperandKind::Reg ? src(1) : RegSet(0); return e;
        case Op::Vld1: case Op::Vld2: case Op::Vld3: case Op::Vld4: case Op::Vst1: case Op::Vst2: case Op::Vst3: case Op::Vst4:
            e.uses = src(1);
            e.defs = mem_writeback(o[1]);
            return e;
        default: break;
    }
    return e;  // (no core register: floating point, Advanced SIMD arithmetic, hints, barriers, it)
}

namespace {

// ---- Symbolic values ---------------------------------------------------------------------------------------------

// 0: unreached; 1: unknown; 2<<56 | k: register k at entry; 3<<56 | c: the entry's sp + c; 4<<56 | site<<8 |
// phase<<4 | k: register k in the context after the call at instruction `site` (phase 2), or stored before it (1),
// or the context's copy of a register an internal entry returned by value (3); 5<<56 | k: what the context held for
// register k when an internal entry was entered without it (its argument; nothing known of the context's copy).
using Value = u64;
constexpr Value kBottom = 0, kTop = 1;
constexpr Value sp_at(i32 c) { return (Value(3) << 56) | static_cast<u32>(c); }
constexpr Value entry_value(u32 k) { return k == arm::kSp ? sp_at(0) : (Value(2) << 56) | k; }
constexpr Value caller_ctx(u32 k) { return (Value(5) << 56) | k; }
constexpr Value fresh(u32 site, u32 k, u32 phase) { return (Value(4) << 56) | (Value(site) << 8) | (phase << 4) | k; }
constexpr bool is_sp(Value v) { return (v >> 56) == 3; }
constexpr i32 sp_offset(Value v) { return static_cast<i32>(static_cast<u32>(v)); }
constexpr bool fresh_of(Value v, u32 site) { return (v >> 56) == 4 && static_cast<u32>(v >> 8) == site; }
constexpr Value join(Value a, Value b) { return a == b ? a : a == kBottom ? b : b == kBottom ? a : kTop; }
constexpr bool same(Value a, Value b) { return a == b && a > kTop; }

struct Slot {
    i32 offset;
    Value value;
    bool saved = false;  // (written by a push or an stmdb sp!: a register's save, which no other code writes)
};

struct State {
    bool reached = false;
    Value local[15] = {};
    Value ctx[15] = {};
    // The registers whose local holds what the context holds, on every path here (what a join of two symbols loses:
    // a register clean on both sides with different values on each).
    RegSet clean = kAllRegisters;
    nx::vector<Slot> slots;  // words at offsets from the entry's sp, by offset
    // A stack address went where the analysis does not follow it (to a callee, into memory, through an operation it
    // does not model): a callee, or a store through a base it does not know, may then write the frame's words. The
    // saves of registers (Slot::saved) are no object's: nothing writes them through such an address.
    bool escaped = false;

    RegSet dirty() const { return static_cast<RegSet>(~clean & kAllRegisters); }
    void forget_frame() {
        std::erase_if(slots, [](const Slot& s) { return !s.saved; });
    }
    void set_local(u32 k, Value v) {
        local[k] = v;
        if (same(v, ctx[k])) clean |= static_cast<RegSet>(1u << k);
        else clean &= static_cast<RegSet>(~(1u << k));
    }
    void invalidate(i64 lo, i64 hi) {
        std::erase_if(slots, [&](const Slot& s) { return s.offset < hi && s.offset + 4 > lo; });
    }
    void store(i32 offset, Value v, bool saved = false) {
        invalidate(offset, i64(offset) + 4);
        const auto it = std::lower_bound(slots.begin(), slots.end(), offset, [](const Slot& s, i32 o) { return s.offset < o; });
        slots.insert(it, Slot{offset, v, saved});
    }
    const Slot* find(i32 offset) const {
        const auto it = std::lower_bound(slots.begin(), slots.end(), offset, [](const Slot& s, i32 o) { return s.offset < o; });
        return it != slots.end() && it->offset == offset ? &*it : nullptr;
    }
    Value load(i32 offset) const {
        const Slot* s = find(offset);
        return s ? s->value : kTop;
    }
    void kill(u32 site) {
        for (u32 k = 0; k < 15; ++k) {
            if (fresh_of(local[k], site)) local[k] = kTop;
            if (fresh_of(ctx[k], site)) ctx[k] = kTop;
        }
        for (Slot& s : slots)
            if (fresh_of(s.value, site)) s.value = kTop;
    }
};

// Joins b into a; whether a changed.
bool join_into(State& a, const State& b) {
    if (!b.reached) return false;
    if (!a.reached) {
        a = b;
        return true;
    }
    bool changed = false;
    for (u32 k = 0; k < 15; ++k) {
        const Value l = join(a.local[k], b.local[k]), c = join(a.ctx[k], b.ctx[k]);
        changed |= l != a.local[k] || c != a.ctx[k];
        a.local[k] = l, a.ctx[k] = c;
    }
    if (const RegSet clean = a.clean & b.clean; clean != a.clean) a.clean = clean, changed = true;
    if (b.escaped && !a.escaped) a.escaped = changed = true;
    // (a word known on one side only, or different, is unknown: dropped; a save only if it is one on both sides)
    nx::vector<Slot> kept;
    for (const Slot& s : a.slots) {
        if (b.load(s.offset) != s.value) continue;
        const Slot* o = b.find(s.offset);
        const bool saved = s.saved && o && o->saved;
        changed |= saved != s.saved;
        kept.push_back(Slot{s.offset, s.value, saved});
    }
    if (kept.size() != a.slots.size()) changed = true;
    a.slots = std::move(kept);
    return changed;
}

// Whether a register other than sp holds a stack address (one a callee, or the kernel, may be given).
bool holds_stack_address(const State& st) {
    for (u32 k = 0; k < 15; ++k)
        if (k != arm::kSp && is_sp(st.local[k])) return true;
    return false;
}

// The register a memory access takes its address from; 16 for an instruction that accesses no memory.
u32 memory_base(const arm::Instruction& in) {
    const auto& o = in.operands;
    switch (in.op) {
        case Op::Ldr: case Op::Str: case Op::Ldrb: case Op::Ldrh: case Op::Ldrsb: case Op::Ldrsh: case Op::Strb: case Op::Strh:
        case Op::Ldrt: case Op::Ldrbt: case Op::Ldrht: case Op::Ldrsbt: case Op::Ldrsht: case Op::Strt: case Op::Strbt: case Op::Strht:
        case Op::Ldrex: case Op::Ldrexb: case Op::Ldrexh: case Op::Vldr: case Op::Vstr:
        case Op::Vld1: case Op::Vld2: case Op::Vld3: case Op::Vld4: case Op::Vst1: case Op::Vst2: case Op::Vst3: case Op::Vst4:
            return o[1].reg;
        case Op::Ldrd: case Op::Strd: case Op::Ldrexd: case Op::Strex: case Op::Strexb: case Op::Strexh: case Op::Swp: case Op::Swpb:
            return o[2].reg;
        case Op::Strexd: return o[3].reg;
        case Op::Push: case Op::Pop: case Op::Vpush: case Op::Vpop: return arm::kSp;
        case Op::Ldm: case Op::Ldmib: case Op::Ldmda: case Op::Ldmdb: case Op::Stm: case Op::Stmib: case Op::Stmda: case Op::Stmdb:
        case Op::Vldmia: case Op::Vldmdb: case Op::Vstmia: case Op::Vstmdb: case Op::Pld: case Op::Pldw: case Op::Pli:
            return o[0].reg;
        default: return 16;
    }
}

// Whether the instruction takes a stack address where the analysis stops following it: stores it to memory, uses it
// as an index, or computes from it what is not a stack address the analysis knows. (One kept in a register is
// followed; given to a call or the kernel there, it escapes: Solver::forward, apply's svc.)
bool escapes(const arm::Instruction& in, const RegisterEffect& eff, const State& st, const Value* nv) {
    RegSet stack = kSpRegister;
    for (u32 k = 0; k < 15; ++k)
        if (is_sp(st.local[k])) stack |= static_cast<RegSet>(1u << k);
    stack &= eff.uses;
    if (!stack) return false;
    if (const u32 base = memory_base(in); base != 16) return (stack & ~(1u << base)) != 0;
    switch (in.op) {
        case Op::Cmp: case Op::Cmn: case Op::Tst: case Op::Teq: case Op::Cbz: case Op::Cbnz: return false;  // (flags, a branch)
        default: break;
    }
    const RegSet defs = static_cast<RegSet>(eff.defs & ~kSpRegister);
    // (sp alone moved: still in the frame; nothing written: into a register the analysis does not see, a VFP one)
    if (!defs) return eff.defs == 0;
    for (u32 k = 0; k < 15; ++k)
        if ((defs & (1u << k)) && !is_sp(nv[k])) return true;
    return false;
}

// The instruction's own effect on the symbolic state: the registers it writes (those it is not modeled for become
// unknown), the stack words it writes, and the value it writes to the PC (a branch's target; kTop if unknown).
void apply(const arm::Instruction& in, const RegisterEffect& eff, State& st, Value& pc) {
    pc = kTop;
    Value nv[16];
    for (Value& v : nv) v = kTop;
    const auto& o = in.operands;
    const u8 n = in.operand_count;
    const auto value = [&](u32 r) { return r < 15 ? st.local[r] : kTop; };
    // A memory operand's base and the address it accesses (when the base is a known stack address and the offset an
    // immediate), and the base's value after it (writeback).
    struct Address {
        bool known = false, stack = false;
        i32 at = 0, after = 0;
    };
    const auto address = [&](const arm::Operand& m) {
        Address a;
        if (m.reg == arm::kPc) return a;
        const Value b = value(m.reg);
        // (through the stack pointer at an unknown offset from the entry's: somewhere in the frame)
        a.stack = is_sp(b) || m.reg == arm::kSp;
        if (!is_sp(b)) return a;
        if (m.flags & (arm::kMemIndexReg | arm::kMemPostReg)) return a;
        const i32 base = sp_offset(b);
        const i32 off = (m.flags & arm::kMemSubtract) ? -static_cast<i32>(m.imm) : static_cast<i32>(m.imm);
        a.known = true;
        a.at = (m.flags & arm::kMemPre) ? base + off : base;
        a.after = base + off;
        return a;
    };
    const auto writeback = [&](const arm::Operand& m, const Address& a) {
        if (!(m.flags & (arm::kMemWriteBack | arm::kMemPostReg)) || m.reg == arm::kPc) return;
        nv[m.reg] = a.known ? sp_at(a.after) : kTop;
    };
    // A store through a base that is not a stack address the analysis knows: once one has escaped, it may be one of
    // the frame's words (not a register's save).
    const auto unknown_store = [&] {
        if (st.escaped) st.forget_frame();
    };
    switch (in.op) {
        case Op::Mov:
            if (n == 2 && o[1].kind == OperandKind::Reg && o[1].reg != arm::kPc) {
                const Value v = value(o[1].reg);
                nv[o[0].reg] = v;
                // (an ARM data-processing write of the PC interworks; Thumb's keeps bit 0 set: not the LR value)
                if (o[0].reg == arm::kPc && in.mode == arm::Mode::Arm) pc = v;
            }
            break;
        case Op::Add:
        case Op::Sub: {
            const arm::Operand& rn = n == 3 ? o[1] : o[0];
            const arm::Operand& op2 = n == 3 ? o[2] : o[1];
            if ((op2.kind == OperandKind::Imm || op2.kind == OperandKind::ModImm) && rn.kind == OperandKind::Reg && is_sp(value(rn.reg))) {
                const i32 c = sp_offset(value(rn.reg));
                nv[o[0].reg] = sp_at(in.op == Op::Add ? c + static_cast<i32>(op2.imm) : c - static_cast<i32>(op2.imm));
            }
            break;
        }
        case Op::Bx: case Op::Bxj: pc = value(o[0].reg); break;
        case Op::Push: case Op::Pop: case Op::Ldm: case Op::Ldmib: case Op::Ldmda: case Op::Ldmdb:
        case Op::Stm: case Op::Stmib: case Op::Stmda: case Op::Stmdb: {
            const bool pp = in.op == Op::Push || in.op == Op::Pop;
            const u32 rn = pp ? arm::kSp : o[0].reg;
            const u32 list = pp ? o[0].imm : o[1].imm;
            const bool load = in.op == Op::Pop || in.op == Op::Ldm || in.op == Op::Ldmib || in.op == Op::Ldmda || in.op == Op::Ldmdb;
            const bool increment = in.op == Op::Pop || in.op == Op::Ldm || in.op == Op::Ldmib || in.op == Op::Stm || in.op == Op::Stmib;
            const bool before = in.op == Op::Push || in.op == Op::Ldmib || in.op == Op::Ldmdb || in.op == Op::Stmib || in.op == Op::Stmdb;
            const bool wb = pp || (in.flags & arm::kWriteBack);
            const i32 count = std::popcount(list);
            const Value b = value(rn);
            // (push, stmdb sp!: the saves of registers)
            const bool saves = !load && rn == arm::kSp && wb && !increment && before;
            if (is_sp(b)) {
                const i32 base = sp_offset(b);
                const i32 first = increment ? (before ? base + 4 : base) : (before ? base - 4 * count : base - 4 * count + 4);
                if (wb) nv[rn] = sp_at(increment ? base + 4 * count : base - 4 * count);
                i32 at = first;
                for (u32 r = 0; r < 16; ++r) {
                    if (!(list & (1u << r))) continue;
                    if (load) {
                        const Value v = st.load(at);
                        if (r == arm::kPc) pc = v;
                        else nv[r] = v;  // (a loaded base overrides its writeback, as the translation does)
                    } else {
                        st.store(at, r == rn ? b : value(r), saves);
                    }
                    at += 4;
                }
            } else if (!load && rn == arm::kSp) {
                st.slots.clear();  // (the stack pointer at an unknown offset from the entry's)
            } else if (!load) {
                unknown_store();
            }
            break;
        }
        case Op::Ldr: case Op::Str: case Op::Ldrb: case Op::Ldrh: case Op::Ldrsb: case Op::Ldrsh: case Op::Strb: case Op::Strh:
        case Op::Ldrt: case Op::Ldrbt: case Op::Ldrht: case Op::Ldrsbt: case Op::Ldrsht: case Op::Strt: case Op::Strbt: case Op::Strht: {
            const arm::Operand& m = o[1];
            const bool load = in.op == Op::Ldr || in.op == Op::Ldrb || in.op == Op::Ldrh || in.op == Op::Ldrsb || in.op == Op::Ldrsh ||
                              in.op == Op::Ldrt || in.op == Op::Ldrbt || in.op == Op::Ldrht || in.op == Op::Ldrsbt || in.op == Op::Ldrsht;
            const bool word = in.op == Op::Ldr || in.op == Op::Str || in.op == Op::Ldrt || in.op == Op::Strt;
            const u32 size = word ? 4 : (in.op == Op::Ldrb || in.op == Op::Strb || in.op == Op::Ldrsb || in.op == Op::Ldrbt ||
                                         in.op == Op::Strbt || in.op == Op::Ldrsbt) ? 1 : 2;
            const Address a = address(m);
            writeback(m, a);
            const u32 rt = o[0].reg;
            if (load) {
                const Value v = word && a.known ? st.load(a.at) : kTop;
                if (rt == arm::kPc) pc = v;
                else nv[rt] = v;
            } else if (a.known) {
                if (word) st.store(a.at, value(rt));
                else st.invalidate(a.at, i64(a.at) + size);
            } else if (a.stack) {
                st.slots.clear();  // (a store at an unknown place in the frame)
            } else {
                unknown_store();
            }
            break;
        }
        case Op::Ldrd: case Op::Strd: {
            const arm::Operand& m = o[2];
            const Address a = address(m);
            writeback(m, a);
            if (in.op == Op::Ldrd) {
                nv[o[0].reg] = a.known ? st.load(a.at) : kTop;
                nv[o[1].reg] = a.known ? st.load(a.at + 4) : kTop;
            } else if (a.known) {
                const Value lo = value(o[0].reg), hi = value(o[1].reg);
                st.store(a.at, lo);
                st.store(a.at + 4, hi);
            } else if (a.stack) {
                st.slots.clear();
            } else {
                unknown_store();
            }
            break;
        }
        case Op::Vstr: {
            const Address a = address(o[1]);
            if (a.known) st.invalidate(a.at, i64(a.at) + (o[0].kind == OperandKind::DReg ? 8 : 4));
            else if (a.stack) st.slots.clear();
            else unknown_store();
            break;
        }
        case Op::Vldmia: case Op::Vldmdb: case Op::Vstmia: case Op::Vstmdb: case Op::Vpush: case Op::Vpop: {
            const bool pp = in.op == Op::Vpush || in.op == Op::Vpop;
            const bool load = in.op == Op::Vldmia || in.op == Op::Vldmdb || in.op == Op::Vpop;
            const bool increment = in.op == Op::Vldmia || in.op == Op::Vstmia || in.op == Op::Vpop;
            const u32 rn = pp ? arm::kSp : o[0].reg;
            const arm::Operand& list = o[pp ? 0 : 1];
            const i32 bytes = static_cast<i32>((list.kind == OperandKind::DRegList ? 8u : 4u) * list.count + ((in.flags & arm::kXForm) ? 4u : 0u));
            const Value b = value(rn);
            if (is_sp(b)) {
                const i32 base = sp_offset(b);
                const i32 lo = increment ? base : base - bytes;
                if (!load) st.invalidate(lo, i64(lo) + bytes);
                if (pp || (in.flags & arm::kWriteBack)) nv[rn] = sp_at(increment ? base + bytes : base - bytes);
            } else if (!load && rn == arm::kSp) {
                st.slots.clear();
            } else if (!load) {
                unknown_store();
            }
            break;
        }
        case Op::Vst1: case Op::Vst2: case Op::Vst3: case Op::Vst4:
            if (is_sp(value(o[1].reg)) || o[1].reg == arm::kSp) st.slots.clear();
            else unknown_store();
            break;
        case Op::Strex: case Op::Strexb: case Op::Strexh: case Op::Swp: case Op::Swpb:
            if (is_sp(value(o[2].reg)) || o[2].reg == arm::kSp) st.slots.clear();
            else unknown_store();
            break;
        case Op::Strexd:
            if (is_sp(value(o[3].reg)) || o[3].reg == arm::kSp) st.slots.clear();
            else unknown_store();
            break;
        case Op::Svc:  // (a call into the kernel, with its arguments in registers)
            if (holds_stack_address(st)) st.escaped = true;
            if (st.escaped) st.forget_frame();
            break;
        default: break;
    }
    if (escapes(in, eff, st, nv)) st.escaped = true;
    for (u32 k = 0; k < 15; ++k)
        if (eff.defs & (1u << k)) st.set_local(k, nv[k]);
}

// The blocks of a graph that are on a cycle (Tarjan's strongly connected components, iteratively).
nx::vector<u8> loop_blocks(const Cfg& g) {
    const u32 n = static_cast<u32>(g.blocks.size());
    nx::vector<u8> in_loop(n, 0);
    nx::vector<u32> index(n, kNoIndex), low(n, 0), stack, path;
    nx::vector<u8> on_stack(n, 0);
    nx::vector<u32> next_succ(n, 0);
    u32 counter = 0;
    for (u32 root = 0; root < n; ++root) {
        if (index[root] != kNoIndex) continue;
        path.push_back(root);
        index[root] = low[root] = counter++;
        stack.push_back(root), on_stack[root] = 1;
        while (!path.empty()) {
            const u32 v = path.back();
            const auto& succ = g.blocks[v].successors;
            if (next_succ[v] < succ.size()) {
                const u32 w = succ[next_succ[v]++];
                if (w == v) in_loop[v] = 1;
                if (index[w] == kNoIndex) {
                    index[w] = low[w] = counter++;
                    stack.push_back(w), on_stack[w] = 1;
                    path.push_back(w);
                } else if (on_stack[w]) {
                    low[v] = std::min(low[v], index[w]);
                }
                continue;
            }
            path.pop_back();
            if (!path.empty()) low[path.back()] = std::min(low[path.back()], low[v]);
            if (low[v] == index[v]) {
                usize size = 0;
                u32 w;
                const usize base = stack.size();
                do {
                    w = stack[stack.size() - 1 - size];
                    ++size;
                } while (w != v);
                for (usize k = base - size; k < base; ++k) {
                    on_stack[stack[k]] = 0;
                    if (size > 1) in_loop[stack[k]] = 1;
                }
                stack.resize(base - size);
            }
        }
    }
    return in_loop;
}

// ---- The solver ----------------------------------------------------------------------------------------------------

struct Fn {
    u32 code = 0;
    const Function* fn = nullptr;
    Cfg cfg;
    nx::vector<RegisterEffect> effect;
    nx::vector<u32> callers;  // functions with a direct call or tail call to this one (indices)
    RegSet preserved = kPreservable;
    bool queued = false;
    // Internal entries (EntryKind): every way out returns to the LR it was entered with (computed with every call as a
    // call through f_<address>), what it and its callees may write, how it is entered, and its signature.
    bool lr_ok = false;
    u32 direct_callers = 0;  // (bl/blx sites calling it)
    RegSet writes = 0;
    EntryKind kind = EntryKind::Public;
    EntryReason reason = EntryReason::None;
    RegSet modified = 0, results = 0, params = 0;
    nx::vector<RegSet> kept;  // (each call's kept registers as the forward pass gives them, before liveness narrows them)
};

class Solver {
public:
    Solver(const Program& program, const RegisterOptions& options) : program_(program), options_(options) {}
    RegisterAnalysis run();

private:
    u32 index(u32 code) const {
        auto it = index_.find(code);
        return it == index_.end() ? kNoIndex : it->second;
    }
    // The callee of a direct call node as the call uses it: its internal entry's kind (Public: through f_<address>).
    const Fn* callee_of(const CfgNode& node) const {
        if (node.call != CallKind::Direct) return nullptr;
        const u32 t = index(node.call_target);
        return t == kNoIndex ? nullptr : &fns_[t];
    }
    EntryKind entry_of(const CfgNode& node) const {
        const Fn* c = callee_of(node);
        return c && options_.internal_calls ? c->kind : EntryKind::Public;
    }
    // The forward pass over f's graph. Returns the registers its exits preserve; with `out`, the per-instruction sets;
    // with `lr_ok`, whether every way out goes back to the LR it was entered with.
    RegSet forward(const Fn& f, bool sp_kept, FunctionRegisters* out, bool* lr_ok_out = nullptr) const;
    void liveness(const Fn& f, FunctionRegisters& r) const;
    void classify();

    const Program& program_;
    const RegisterOptions& options_;
    nx::vector<Fn> fns_;
    nx::flat_hash_map<u32, u32> index_;
};

RegSet Solver::forward(const Fn& f, bool sp_kept, FunctionRegisters* out, bool* lr_ok_out) const {
    const Cfg& g = f.cfg;
    const usize nb = g.blocks.size();
    nx::vector<State> at(nb);
    State init;
    init.reached = true;
    // (an internal entry with nothing outside in its reach is entered with its arguments in C arguments, the context's
    // copies of them whatever its caller's context held: dirty from the start)
    const bool internal = options_.internal_calls && f.kind != EntryKind::Public;
    const auto ctx_at_entry = [&](u32 k) {
        return f.kind == EntryKind::Inside && options_.internal_calls && (kArgumentRegisters & (1u << k)) ? caller_ctx(k) : entry_value(k);
    };
    for (u32 k = 0; k < 15; ++k) init.local[k] = entry_value(k), init.ctx[k] = ctx_at_entry(k);
    for (u32 k = 0; k < 15; ++k)
        if (!same(init.local[k], init.ctx[k])) init.clean &= static_cast<RegSet>(~(1u << k));
    // What an exit writes to the context: every dirty register, as before; an internal entry's returns leave out what
    // they return by value, and the argument registers the function neither may change nor had changed in the
    // context by a callee (its caller has them; the context's copy is the caller's business).
    const auto exit_set = [&](const State& s) -> RegSet {
        RegSet d = s.dirty();
        if (!internal) return d;
        d &= static_cast<RegSet>(~f.results);
        for (u32 k = 0; k < 15; ++k) {
            const RegSet b = static_cast<RegSet>(1u << k);
            if ((d & b) && (kArgumentRegisters & b) && !(f.modified & b) && s.ctx[k] == ctx_at_entry(k)) d &= static_cast<RegSet>(~b);
        }
        return d;
    };
    const u32 entry_block = g.block_of[g.entry];
    at[entry_block] = init;
    RegSet ok = kPreservable;
    bool lr_ok = true;  // (FunctionRegisters::returns_to_lr)
    // (the blocks inside a loop: a strongly connected component with a cycle)
    nx::vector<u8> in_loop;
    if (out && options_.keep == KeepPolicy::Loops) in_loop = loop_blocks(g);
    const auto kept_by = [&](const CfgNode& node) -> RegSet {
        if (!options_.callee_saved) return 0;
        if (node.call == CallKind::Direct) {
            const u32 t = index(node.call_target);
            return t == kNoIndex ? RegSet(0) : fns_[t].preserved;
        }
        return sp_kept ? kSpRegister : RegSet(0);
    };
    // An exit of the function (an edge out of it) with the state there: what it preserves.
    const auto exit = [&](const Edge& e, const State& s, Value pc) {
        if (!s.reached) return;
        const auto entry_values = [&] {
            RegSet r = 0;
            for (u32 k = 4; k < 14; ++k)
                if ((kPreservable & (1u << k)) && s.local[k] == entry_value(k)) r |= static_cast<RegSet>(1u << k);
            return r;
        };
        switch (e.kind) {
            case EdgeKind::Return:
            case EdgeKind::IndirectJump:
                // (a return when the target is the LR the function was entered with, which is its return address
                // when it was called directly; else a jump through the runtime, to code that may do anything)
                ok &= pc == entry_value(arm::kLr) ? entry_values() : RegSet(0);
                lr_ok &= pc == entry_value(arm::kLr);
                break;
            case EdgeKind::TailCall: {
                const u32 t = index(e.to);
                ok &= t != kNoIndex && s.local[arm::kLr] == entry_value(arm::kLr) ? static_cast<RegSet>(entry_values() & fns_[t].preserved)
                                                                                 : RegSet(0);
                lr_ok &= t != kNoIndex && s.local[arm::kLr] == entry_value(arm::kLr);
                break;
            }
            case EdgeKind::ImportTail: ok = 0, lr_ok = false; break;
            default: break;
        }
    };
    // Runs one instruction from `s`: the state after it on its executed edges (`post`), and the state the
    // continuation after its translation sees (`after`). With `out`, its sets.
    const auto step = [&](u32 i, const State& s, State& post, State& after, bool record) {
        const CfgNode& node = g.nodes[i];
        const arm::Instruction& in = f.fn->code[i];
        const RegisterEffect& eff = f.effect[i];
        if (record && eff.traps) out->trap_store[i] = s.dirty();
        post = s;
        Value pc = kTop;
        apply(in, eff, post, pc);
        if (node.call != CallKind::None) {
            // (a stack address in a register the callee is given, or one it saves: it may write the frame through it,
            // or through one given before; below sp is the callee's own frame)
            if (holds_stack_address(post)) post.escaped = true;
            if (post.escaped) post.forget_frame();
            if (is_sp(post.local[arm::kSp])) {
                const i32 top = sp_offset(post.local[arm::kSp]);
                std::erase_if(post.slots, [&](const Slot& s) { return s.offset < top; });
            }
            post.kill(i);
            const EntryKind ck = entry_of(node);
            const Fn* callee = ck == EntryKind::Public ? nullptr : callee_of(node);
            // (a callee with nothing outside in its reach gets r0-r3, sp and lr as C arguments: only the other dirty
            // registers go to the context, which it reads them from)
            const RegSet store = ck == EntryKind::Inside ? static_cast<RegSet>(post.dirty() & ~kArgumentRegisters) : post.dirty();
            for (u32 k = 0; k < 15; ++k) {
                if (!(store & (1u << k))) continue;
                if (post.local[k] == kTop) post.local[k] = fresh(i, k, 1);
                post.ctx[k] = post.local[k];
                post.clean |= static_cast<RegSet>(1u << k);
            }
            RegSet keep = 0;
            if (ck == EntryKind::Inside) {
                // What it returns by value is in the local (the context's copy is what it was: nothing inside writes
                // it); what else it may change it left in the context; the rest it does not change, though where it or
                // its callees write such a register it may have put the value it was given back into the context.
                for (u32 k = 0; k < 15; ++k) {
                    const RegSet b = static_cast<RegSet>(1u << k);
                    if (callee->results & b) post.local[k] = fresh(i, k, 2);
                    else if (callee->modified & b) post.local[k] = post.ctx[k] = fresh(i, k, 2);
                    else if (!(post.clean & b) && (callee->writes & b)) post.ctx[k] = kTop;
                    if (same(post.local[k], post.ctx[k]) || ((post.clean & b) && !(callee->modified & b))) post.clean |= b;
                    else post.clean &= static_cast<RegSet>(~b);
                }
                keep = static_cast<RegSet>(~callee->modified & kAllRegisters);
            } else {
                post.clean = kAllRegisters;
                // (an internal entry that may reach outside leaves the context exact but for what it returns by value)
                const RegSet kept = ck == EntryKind::Flushed ? static_cast<RegSet>(~callee->modified & kAllRegisters) : kept_by(node);
                for (u32 k = 0; k < 15; ++k)
                    if (!(kept & (1u << k))) post.local[k] = post.ctx[k] = fresh(i, k, 2);
                if (ck == EntryKind::Flushed)
                    for (u32 k = 0; k < 15; ++k)
                        if (callee->results & (1u << k)) {
                            post.ctx[k] = fresh(i, k, 3);
                            post.clean &= static_cast<RegSet>(~(1u << k));
                        }
                // (known the same either way; kept in the local, or read back, as the policy says; sp after a call to
                // code outside the module's view always kept, and checked)
                keep = kept;
                if (record && node.call != CallKind::Direct && !out->checks_sp) keep = 0;
                if (record && node.call == CallKind::Direct && ck == EntryKind::Public) {
                    if (options_.keep == KeepPolicy::None) keep = 0;
                    else if (options_.keep == KeepPolicy::Loops && !in_loop[g.block_of[i]]) keep = 0;
                }
            }
            if (record) out->call_store[i] = store, out->call_kept[i] = keep, out->call_entry[i] = static_cast<u8>(ck);
        } else if (record) {
            out->exit_store[i] = exit_set(post);
        }
        // The exits inside the translation, and the continuation after it.
        const Edge* cont = nullptr;
        for (u32 k = 0; k < node.edge_count; ++k) {
            const Edge& e = g.edges[node.first_edge + k];
            if (k == node.cont_edge) cont = &e;
            else exit(e, post, pc);
        }
        after = State{};
        if (cont || node.conditional) {
            if (cont) after = post;
            if (node.conditional) join_into(after, s);
            const Edge& c = node.conditional ? node.skip : *cont;
            if (c.kind != EdgeKind::Next) {
                exit(c, after, kTop);
                if (record) out->after_store[i] = after.dirty();
            }
        }
    };
    // Forward over the blocks until nothing changes; then (with `out`) once more, recording.
    nx::vector<u32> work;
    nx::vector<u8> queued(nb, 0);
    work.push_back(entry_block);
    queued[entry_block] = 1;
    State post, after;
    // Each Next edge of instruction i with the state it carries: an executed edge the state after the instruction
    // (`post`), the continuation (and the skip edge) the state after its translation (`after`).
    const auto edges_out = [&](u32 i, auto&& to) {
        const CfgNode& node = g.nodes[i];
        for (u32 k = 0; k < node.edge_count; ++k) {
            const Edge& e = g.edges[node.first_edge + k];
            if (e.kind == EdgeKind::Next) to(e.to, k == node.cont_edge ? after : post);
        }
        if (node.conditional && node.skip.kind == EdgeKind::Next) to(node.skip.to, after);
    };
    const auto run_block = [&](u32 b, bool record) {
        State s = at[b];
        const BasicBlock& blk = g.blocks[b];
        for (u32 i = blk.first; i <= blk.last; ++i) {
            step(i, s, post, after, record);
            if (i == blk.last) break;
            // (inside a block every edge of an instruction goes to the next one)
            State next;
            edges_out(i, [&](u32, const State& from) { join_into(next, from); });
            s = std::move(next);
        }
        edges_out(blk.last, [&](u32 to_index, const State& from) {
            const u32 to = g.block_of[to_index];
            if (join_into(at[to], from) && !record && !queued[to]) queued[to] = 1, work.push_back(to);
        });
    };
    while (!work.empty()) {
        const u32 b = work.back();
        work.pop_back();
        queued[b] = 0;
        run_block(b, false);
    }
    if (out) {
        ok = kPreservable;
        lr_ok = true;
        for (u32 b = 0; b < nb; ++b)
            if (at[b].reached) run_block(b, true);
        out->returns_to_lr = lr_ok;
    }
    if (lr_ok_out) *lr_ok_out = lr_ok;
    return ok;
}

void Solver::liveness(const Fn& f, FunctionRegisters& r) const {
    const Cfg& g = f.cfg;
    const usize n = g.nodes.size();
    nx::vector<RegSet>& live = r.live_before;
    live.assign(n, 0);
    r.call_load.assign(n, 0);
    if (f.kept.size() == n) r.call_kept = f.kept;  // (liveness may run again: from the forward pass' sets)
    const bool internal = options_.internal_calls && f.kind != EntryKind::Public;
    for (bool changed = true; changed;) {
        changed = false;
        for (usize i = n; i-- > 0;) {
            const CfgNode& node = g.nodes[i];
            RegisterEffect eff = f.effect[i];
            const auto edge_use = [&](const Edge& e, bool is_cont) -> RegSet {
                if (e.kind == EdgeKind::Next) return live[e.to];
                if (is_cont) return r.after_store[i];
                if (e.kind == EdgeKind::Trap) return RegSet(0);
                // (an internal entry's returns give back what it returns by value)
                return internal ? static_cast<RegSet>(r.exit_store[i] | f.results) : r.exit_store[i];
            };
            RegSet exec = 0;
            bool returns = node.edge_count > 0;
            for (u32 k = 0; k < node.edge_count; ++k) {
                const Edge& e = g.edges[node.first_edge + k];
                exec |= edge_use(e, k == node.cont_edge);
                if (e.kind != EdgeKind::Return && e.kind != EdgeKind::IndirectJump) returns = false;
            }
            // (an internal entry returns to its caller without reading where to: every way out goes back to the LR it was
            // entered with; the register a bx or mov pc reads the address from is not read)
            if (internal && returns && node.call == CallKind::None) {
                const arm::Instruction& in = f.fn->code[i];
                if ((in.op == Op::Bx || in.op == Op::Bxj || (in.op == Op::Mov && in.operand_count == 2)) && in.operands[in.op == Op::Mov ? 1 : 0].kind == OperandKind::Reg)
                    eff.uses &= static_cast<RegSet>(~bit(in.operands[in.op == Op::Mov ? 1 : 0].reg));
            }
            if (node.call != CallKind::None) {
                const EntryKind ck = static_cast<EntryKind>(r.call_entry[i]);
                const Fn* callee = ck == EntryKind::Public ? nullptr : callee_of(node);
                const RegSet results = callee ? callee->results : RegSet(0);
                r.call_load[i] = static_cast<RegSet>(exec & ~r.call_kept[i] & ~results);
                // (the stack pointer checked after a call to code outside the module's view: read there)
                const RegSet checked = r.checks_sp && node.call != CallKind::Direct ? static_cast<RegSet>(r.call_kept[i] & kSpRegister) : RegSet(0);
                // (an internal entry's arguments are read from the locals)
                exec = static_cast<RegSet>(r.call_store[i] | (exec & r.call_kept[i]) | checked | (callee ? callee->params : RegSet(0)));
            }
            RegSet b = static_cast<RegSet>(eff.uses | (eff.traps ? r.trap_store[i] : RegSet(0)) |
                                           (node.conditional ? exec : RegSet(exec & ~eff.defs)));
            if (node.conditional) b |= edge_use(node.skip, true);
            if (b != live[i]) live[i] = b, changed = true;
        }
    }
    // (the registers kept across a call: those live after it, and the stack pointer a check reads)
    for (usize i = 0; i < n; ++i) {
        const CfgNode& node = g.nodes[i];
        if (node.call == CallKind::None) continue;
        RegSet exec = 0;
        for (u32 k = 0; k < node.edge_count; ++k) {
            const Edge& e = g.edges[node.first_edge + k];
            exec |= e.kind == EdgeKind::Next ? live[e.to] : k == node.cont_edge ? r.after_store[i] : RegSet(0);
        }
        if (r.checks_sp && node.call != CallKind::Direct) exec |= kSpRegister;
        r.call_kept[i] &= exec;
    }
    r.load_at_entry = g.entry == kNoIndex ? RegSet(0) : live[g.entry];
    // (an internal entry: r0-r3, sp and lr are its C arguments, the others it reads from the context)
    r.params = internal ? static_cast<RegSet>(r.load_at_entry & kArgumentRegisters) : RegSet(0);
    if (internal) r.load_at_entry &= static_cast<RegSet>(~kArgumentRegisters);
    RegSet touched = static_cast<RegSet>(r.load_at_entry | r.params);
    for (usize i = 0; i < n; ++i) {
        touched |= static_cast<RegSet>(f.effect[i].uses | f.effect[i].defs | r.trap_store[i] | r.call_store[i] | r.call_load[i] |
                                       r.call_kept[i] | r.exit_store[i] | r.after_store[i]);
        if (static_cast<EntryKind>(r.call_entry[i]) != EntryKind::Public)
            if (const Fn* c = callee_of(g.nodes[i])) touched |= static_cast<RegSet>(c->params | c->results);
    }
    if (internal) touched |= f.results;
    r.touched = touched;
}

// Which functions get an internal entry, and of what kind (EntryKind): what each may write (a least fixed point over
// the module's calls), whether anything it may reach is code outside the generated code's view (another least fixed
// point), and so what it may change and returns by value.
void Solver::classify() {
    const u32 nf = static_cast<u32>(fns_.size());
    nx::flat_hash_set<u32> excluded;
    if (options_.no_internal)
        for (u32 c : *options_.no_internal) excluded.insert(c & ~1u);
    // What each function and its callees may write: everything when it reaches code outside the module's view.
    for (Fn& f : fns_) {
        RegSet w = 0;
        for (usize i = 0; i < f.effect.size(); ++i) w |= f.effect[i].defs;
        for (const CfgNode& node : f.cfg.nodes) {
            if (node.call == CallKind::Import || node.call == CallKind::Indirect || node.call == CallKind::System) w = kAllRegisters;
            const auto edge = [&](const Edge& e) {
                if (e.kind == EdgeKind::IndirectJump || e.kind == EdgeKind::ImportTail) w = kAllRegisters;
            };
            for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e) edge(*e);
            if (node.conditional) edge(node.skip);
        }
        if (!f.fn->code.empty() && f.cfg.entry == kNoIndex) w = kAllRegisters;
        f.writes = w;
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (Fn& f : fns_) {
            RegSet w = f.writes;
            const auto from = [&](u32 code) {
                const u32 t = index(code);
                w |= t == kNoIndex ? kAllRegisters : fns_[t].writes;
            };
            for (const CfgNode& node : f.cfg.nodes) {
                if (node.call == CallKind::Direct) from(node.call_target);
                for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e)
                    if (e->kind == EdgeKind::TailCall) from(e->to);
                if (node.conditional && node.skip.kind == EdgeKind::TailCall) from(node.skip.to);
            }
            if (w != f.writes) f.writes = w, changed = true;
        }
    }
    // Which functions can have an internal entry: every way out a return to the LR it was entered with, no tail call.
    nx::vector<u8> eligible(nf, 0);
    for (u32 i = 0; i < nf; ++i) {
        Fn& f = fns_[i];
        EntryReason why = EntryReason::None;
        if (!options_.internal_calls) why = EntryReason::Disabled;
        else if (excluded.contains(f.code & ~1u)) why = EntryReason::Excluded;
        else if (f.fn->code.empty() || f.cfg.entry == kNoIndex) why = EntryReason::NoCode;
        else if (!f.fn->returns) why = EntryReason::NoReturn;
        else if (f.direct_callers == 0) why = EntryReason::NoCallers;
        else if (!f.lr_ok) why = EntryReason::OtherReturn;
        if (why == EntryReason::None)
            for (const CfgNode& node : f.cfg.nodes) {
                const auto edge = [&](const Edge& e) {
                    if (e.kind == EdgeKind::TailCall || e.kind == EdgeKind::ImportTail) why = EntryReason::TailCall;
                };
                for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e) edge(*e);
                if (node.conditional) edge(node.skip);
            }
        f.reason = why;
        eligible[i] = why == EntryReason::None;
    }
    // What reaches code outside the generated code's view: its own calls to imports, through the runtime, svc, traps;
    // a call to a function without an internal entry (entered with every register in the context); a callee that does.
    nx::vector<u8> outside(nf, 0);
    for (u32 i = 0; i < nf; ++i) {
        if (!eligible[i]) continue;
        Fn& f = fns_[i];
        EntryReason why = EntryReason::None;
        for (usize k = 0; k < f.cfg.nodes.size() && why == EntryReason::None; ++k) {
            const CfgNode& node = f.cfg.nodes[k];
            if (node.call == CallKind::Import) why = EntryReason::Import;
            else if (node.call == CallKind::Indirect) why = EntryReason::Indirect;
            else if (node.call == CallKind::System) why = EntryReason::Svc;
            else if (f.effect[k].traps) why = EntryReason::Trap;
            else if (node.call == CallKind::Direct) {
                const u32 t = index(node.call_target);
                if (t == kNoIndex || !eligible[t]) why = EntryReason::PublicCallee;
            }
            const auto edge = [&](const Edge& e) {
                if (e.kind == EdgeKind::Trap && why == EntryReason::None) why = EntryReason::Trap;
            };
            for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e) edge(*e);
            if (node.conditional) edge(node.skip);
        }
        if (why != EntryReason::None) outside[i] = 1, f.reason = why;
    }
    for (bool changed = true; changed;) {
        changed = false;
        for (u32 i = 0; i < nf; ++i) {
            if (!eligible[i] || outside[i]) continue;
            for (const CfgNode& node : fns_[i].cfg.nodes) {
                if (node.call != CallKind::Direct) continue;
                const u32 t = index(node.call_target);
                if (t != kNoIndex && outside[t]) {
                    outside[i] = 1, fns_[i].reason = EntryReason::ViaCallee, changed = true;
                    break;
                }
            }
        }
    }
    for (u32 i = 0; i < nf; ++i) {
        Fn& f = fns_[i];
        f.kind = !eligible[i] ? EntryKind::Public : outside[i] ? EntryKind::Flushed : EntryKind::Inside;
        f.modified = f.kind == EntryKind::Public ? kAllRegisters : static_cast<RegSet>(f.writes & ~f.preserved & kAllRegisters);
        f.results = f.kind == EntryKind::Public ? RegSet(0) : static_cast<RegSet>(f.modified & kResultRegisters);
        f.params = 0;
    }
}

RegisterAnalysis Solver::run() {
    RegisterAnalysis out;
    out.callee_saved = options_.callee_saved;
    for (const auto& [code, fn] : program_.functions) {
        index_[code] = static_cast<u32>(fns_.size());
        Fn f;
        f.code = code;
        f.fn = &fn;
        fns_.push_back(std::move(f));
    }
    for (Fn& f : fns_) {
        f.cfg = build_cfg(program_, *f.fn);
        const usize n = f.fn->code.size();
        const nx::vector<RegisterEffect>* given = nullptr;
        if (options_.effects)
            if (auto it = options_.effects->find(f.code); it != options_.effects->end() && it->second.size() == n) given = &it->second;
        f.effect.resize(n);
        for (usize k = 0; k < n; ++k) f.effect[k] = given ? (*given)[k] : register_effect(f.fn->code[k]);
        // (no code: a function that changes nothing; code without its entry: nothing known)
        f.preserved = n == 0 ? kPreservable : f.cfg.entry == kNoIndex ? RegSet(0) : kPreservable;
    }
    for (u32 i = 0; i < fns_.size(); ++i) {
        const Fn& f = fns_[i];
        nx::flat_hash_set<u32> callees;
        for (const CfgNode& node : f.cfg.nodes) {
            if (node.call == CallKind::Direct) {
                callees.insert(index(node.call_target));
                if (const u32 t = index(node.call_target); t != kNoIndex) ++fns_[t].direct_callers;
            }
            for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e)
                if (e->kind == EdgeKind::TailCall) callees.insert(index(e->to));
            if (node.conditional && node.skip.kind == EdgeKind::TailCall) callees.insert(index(node.skip.to));
        }
        nx::vector<u32> sorted(callees.begin(), callees.end());
        std::sort(sorted.begin(), sorted.end());
        for (u32 c : sorted)
            if (c != kNoIndex) fns_[c].callers.push_back(i);
    }
    // What each function preserves: the greatest fixed point (each starts with all, and loses what a pass over its
    // code with its callees' current sets cannot show).
    if (options_.callee_saved) {
        nx::vector<u32> work;
        for (u32 i = fns_.size(); i-- > 0;) work.push_back(i), fns_[i].queued = true;
        while (!work.empty()) {
            const u32 i = work.back();
            work.pop_back();
            Fn& f = fns_[i];
            f.queued = false;
            if (f.fn->code.empty() || f.cfg.entry == kNoIndex) continue;
            const RegSet p = static_cast<RegSet>(forward(f, true, nullptr) & f.preserved);
            if (p == f.preserved) continue;
            f.preserved = p;
            for (u32 c : f.callers)
                if (!fns_[c].queued) fns_[c].queued = true, work.push_back(c);
        }
    } else {
        for (Fn& f : fns_) f.preserved = 0;
    }
    // (a function others rely on checks what its own proof assumed of code outside the module's view; without the
    // check, its proof assumes it all the same and its own code reads sp back)
    // (with internal entries, also a function with an internal entry: its returns find the LR in its frame through sp)
    const auto checks_sp = [&](const Fn& f) {
        return options_.check_sp && options_.callee_saved && !f.callers.empty() &&
               (f.preserved != 0 || (options_.internal_calls && f.kind != EntryKind::Public));
    };
    const auto sp_kept = [&](const Fn& f) { return options_.check_sp ? checks_sp(f) : options_.callee_saved; };
    // Whether every way out goes back to the LR it was entered with, every call taken as a call through f_<address>
    // and sp kept across calls to code outside the module's view where it may be checked (a function given an internal
    // entry on this checks it); then the entries.
    out.internal_calls = options_.internal_calls;
    for (Fn& f : fns_)
        if (!f.fn->code.empty() && f.cfg.entry != kNoIndex)
            (void)forward(f, options_.check_sp ? options_.callee_saved && !f.callers.empty() : options_.callee_saved, nullptr, &f.lr_ok);
    classify();
    for (Fn& f : fns_) {
        FunctionRegisters& r = out.functions[f.code];
        const usize n = f.fn->code.size();
        r.preserved = f.preserved;
        r.trap_store.assign(n, 0);
        r.call_store.assign(n, 0);
        r.call_kept.assign(n, 0);
        r.exit_store.assign(n, 0);
        r.after_store.assign(n, 0);
        r.call_entry.assign(n, 0);
        r.entry = f.kind;
        r.reason = f.reason;
        r.modified = f.modified;
        r.results = f.results;
        r.writes = f.writes;
        if (n == 0 || f.cfg.entry == kNoIndex) {
            r.call_load.assign(n, 0);
            r.live_before.assign(n, 0);
            continue;
        }
        r.checks_sp = checks_sp(f);
        forward(f, sp_kept(f), &r);
        f.kept = r.call_kept;
    }
    // The registers live in each function; an internal entry's arguments are those of r0-r3, sp and lr live at its
    // entry, which its callers' liveness reads: a least fixed point over the module's calls.
    nx::vector<u32> work;
    for (u32 i = fns_.size(); i-- > 0;) work.push_back(i), fns_[i].queued = true;
    while (!work.empty()) {
        const u32 i = work.back();
        work.pop_back();
        Fn& f = fns_[i];
        f.queued = false;
        if (f.fn->code.empty() || f.cfg.entry == kNoIndex) continue;
        FunctionRegisters& r = out.functions[f.code];
        liveness(f, r);
        if (r.params == f.params) continue;
        f.params = r.params;
        for (u32 c : f.callers)
            if (!fns_[c].queued) fns_[c].queued = true, work.push_back(c);
    }
    return out;
}

}  // namespace

RegisterAnalysis register_analysis(const Program& program, const RegisterOptions& options) {
    RegisterAnalysis out = Solver(program, options).run();
    // The cross-check against the exception index table.
    if (program.image && options.callee_saved) {
        const auto restored = unwind_restored(*program.image, &out.unwind.undecoded);
        for (const auto& [code, fn] : program.functions) {
            const auto it = restored.find(code & ~1u);
            if (it == restored.end() || fn.code.empty()) continue;
            ++out.unwind.entries;
            RegSet written = 0;
            const nx::vector<RegisterEffect>* given = nullptr;
            if (options.effects)
                if (auto e = options.effects->find(code); e != options.effects->end() && e->second.size() == fn.code.size()) given = &e->second;
            for (usize i = 0; i < fn.code.size(); ++i) written |= given ? (*given)[i].defs : register_effect(fn.code[i]).defs;
            const RegSet preserved = out.functions[code].preserved & kCalleeSaved;
            if (preserved & written & ~it->second) {
                ++out.unwind.disagree;
                if (out.unwind.disagreeing.size() < 10) out.unwind.disagreeing.push_back(code);
            } else {
                ++out.unwind.agree;
            }
            if (it->second & kCalleeSaved & ~preserved) ++out.unwind.unproven;
        }
    }
    return out;
}

nx::map<u32, RegSet> unwind_restored(const Image& image, usize* undecoded) {
    nx::map<u32, RegSet> out;
    usize bad = 0;
    const auto& info = image.module().info;
    const auto prel31 = [](u32 at, u32 word) { return at + ((word & 0x7FFFFFFFu) | ((word & 0x40000000u) << 1)); };
    if (info.exidx_start && info.exidx_end > info.exidx_start)
        for (u32 at = info.exidx_start; at + 8 <= info.exidx_end; at += 8) {
            const u32 fn = prel31(at, image.u32_at(at)) & ~1u;
            const u32 data = image.u32_at(at + 4);
            nx::vector<u8> ops;
            const auto bytes_of = [&](u32 word, int from) {  // (the word's bytes from bit 31 down, from byte `from`)
                for (int b = from; b < 4; ++b) ops.push_back(static_cast<u8>(word >> (24 - 8 * b)));
            };
            if (data == 1) {  // EXIDX_CANTUNWIND
                ++bad;
                continue;
            }
            if (data & 0x80000000u) {  // inline, compact model 0
                if (((data >> 24) & 0xF) != 0) {
                    ++bad;
                    continue;
                }
                bytes_of(data, 1);
            } else {
                const u32 tab = prel31(at + 4, data);
                if (!image.contains(tab, 8)) {
                    ++bad;
                    continue;
                }
                const u32 w0 = image.u32_at(tab);
                u32 extra = 0, first = 0;
                if (w0 & 0x80000000u) {  // compact model 0, 1 or 2 in the table
                    const u32 index = (w0 >> 24) & 0xF;
                    if (index == 0) bytes_of(w0, 1);
                    else if (index == 1 || index == 2) extra = (w0 >> 16) & 0xFF, bytes_of(w0, 2), first = tab + 4;
                    else {
                        ++bad;
                        continue;
                    }
                } else {  // a personality routine; its data starts as model 1's
                    const u32 w1 = image.u32_at(tab + 4);
                    extra = (w1 >> 24) & 0xFF, bytes_of(w1, 1), first = tab + 8;
                }
                for (u32 k = 0; k < extra; ++k) bytes_of(image.u32_at(first + 4 * k), 0);
            }
            RegSet r = 0;
            bool ok = true, frame = false;
            for (usize i = 0; i < ops.size() && ok;) {
                const u8 op = ops[i++];
                if (op != 0xB0) frame = true;
                if ((op & 0x80) == 0) continue;  // vsp += / -=
                if ((op & 0xF0) == 0x80) {       // pop {r4-r15} under a mask
                    if (i >= ops.size()) ok = false;
                    else {
                        const u32 mask = ((op & 0xFu) << 8) | ops[i++];
                        if (!mask) ok = false;  // (refuse to unwind)
                        r |= static_cast<RegSet>((mask << 4) & kAllRegisters);
                    }
                } else if ((op & 0xF0) == 0x90) {  // vsp = r[n]
                    if ((op & 0xF) == 13 || (op & 0xF) == 15) ok = false;
                } else if ((op & 0xF0) == 0xA0) {  // pop r4-r[4+n] (and r14)
                    for (u32 k = 4; k <= 4u + (op & 7u); ++k) r |= static_cast<RegSet>(1u << k);
                    if (op & 8) r |= kLrRegister;
                } else if (op == 0xB0) {
                    break;  // finish
                } else if (op == 0xB1) {  // pop r0-r3 under a mask
                    if (i >= ops.size()) ok = false;
                    else r |= static_cast<RegSet>(ops[i++] & 0xF);
                } else if (op == 0xB2) {  // vsp += 0x204 + (uleb128 << 2)
                    while (i < ops.size() && (ops[i] & 0x80)) ++i;
                    ++i;
                } else if (op == 0xB3 || op == 0xC6 || op == 0xC7 || op == 0xC8 || op == 0xC9) {
                    ++i;  // (VFP and iWMMXt pops with a byte of operands)
                } else if ((op & 0xF8) == 0xB8 || (op & 0xF8) == 0xC0 || (op & 0xF8) == 0xD0) {
                    // (VFP and iWMMXt pops)
                } else {
                    ok = false;  // (spare or reserved)
                }
            }
            // (an entry of nothing but "finish" describes no frame: what toolchains give a function that calls
            // nothing, which no exception can unwind through, whatever it pushes)
            if (!ok || !frame) {
                ++bad;
                continue;
            }
            out[fn] = r;
        }
    if (undecoded) *undecoded = bad;
    return out;
}

}  // namespace psvrecore::analysis
