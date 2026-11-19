#include "psvrecore/analysis/flags.hpp"

#include <algorithm>

#include "psvrecore/analysis/flow.hpp"
#include "psvrecore/formats/module.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/containers/hash_set.h"

namespace psvrecore::analysis {

using arm::Op;
using arm::OperandKind;

nx::string flag_names(FlagSet f) {
    nx::string s = "----";
    if (f & kFlagN) s[0] = 'n';
    if (f & kFlagZ) s[1] = 'z';
    if (f & kFlagC) s[2] = 'c';
    if (f & kFlagV) s[3] = 'v';
    return s;
}

FlagSet condition_flags(arm::Cond c) {
    switch (c) {
        case arm::Cond::Eq: case arm::Cond::Ne: return kFlagZ;
        case arm::Cond::Cs: case arm::Cond::Cc: return kFlagC;
        case arm::Cond::Mi: case arm::Cond::Pl: return kFlagN;
        case arm::Cond::Vs: case arm::Cond::Vc: return kFlagV;
        case arm::Cond::Hi: case arm::Cond::Ls: return kFlagC | kFlagZ;
        case arm::Cond::Ge: case arm::Cond::Lt: return kFlagN | kFlagV;
        case arm::Cond::Gt: case arm::Cond::Le: return kFlagZ | kFlagN | kFlagV;
        default: return 0;
    }
}

FlagEffect flag_effect(const arm::Instruction& in) {
    FlagEffect e;
    // (an IT instruction's condition is its block's: each instruction in the block tests its own)
    if (in.op != Op::It) e.uses |= condition_flags(in.cond);
    // A rotate right with extend reads the carry: as an operand, or as a memory operand's offset.
    for (u8 k = 0; k < in.operand_count; ++k) {
        const auto& o = in.operands[k];
        if ((o.kind == OperandKind::RegShiftImm || (o.kind == OperandKind::Mem && (o.flags & arm::kMemIndexReg))) &&
            o.shift == arm::ShiftType::Rrx)
            e.uses |= kFlagC;
    }
    const bool s = (in.flags & arm::kSetsFlags) != 0;
    switch (in.op) {
        case Op::Adc: case Op::Sbc: case Op::Rsc:
            e.uses |= kFlagC;
            if (s) e.defs = kAllFlags;
            break;
        case Op::Add: case Op::Sub: case Op::Rsb:
            if (s) e.defs = kAllFlags;
            break;
        case Op::Cmp: case Op::Cmn: e.defs = kAllFlags; break;
        case Op::And: case Op::Eor: case Op::Orr: case Op::Bic: case Op::Orn: case Op::Mov: case Op::Mvn:
        case Op::Tst: case Op::Teq: {
            if (!s && in.op != Op::Tst && in.op != Op::Teq) break;
            e.defs = e.kills = kFlagN | kFlagZ;
            // The carry: the shifter's (a shifted register, a modified immediate that sets it), else unchanged.
            const auto& op2 = in.operands[in.operand_count - 1];
            if (op2.kind == OperandKind::RegShiftImm) e.defs |= kFlagC, e.kills |= kFlagC;
            else if (op2.kind == OperandKind::RegShiftReg) e.defs |= kFlagC, e.kills &= ~kFlagC;  // (a shift by 0 keeps it)
            else if ((op2.kind == OperandKind::Imm || op2.kind == OperandKind::ModImm) && (in.flags & arm::kImmCarry))
                e.defs |= kFlagC, e.kills |= kFlagC;
            return e;
        }
        case Op::Lsl: case Op::Lsr: case Op::Asr: case Op::Ror:
            if (!s) break;
            e.defs = e.kills = kFlagN | kFlagZ | kFlagC;
            // (by a register: a shift by 0 keeps the carry)
            if (!(in.operand_count == 3 && in.operands[2].kind == OperandKind::Imm)) e.kills &= ~kFlagC;
            return e;
        case Op::Rrx:
            e.uses |= kFlagC;
            if (s) e.defs = kFlagN | kFlagZ | kFlagC;
            break;
        case Op::Mul: case Op::Mla: case Op::Mls: case Op::Umull: case Op::Umlal: case Op::Smull: case Op::Smlal:
        case Op::Umaal:
            if (s) e.defs = kFlagN | kFlagZ;
            break;
        case Op::Mrs: e.uses |= kAllFlags; break;
        case Op::Msr:
            // (MSR APSR_nzcvq writes the flags; the GE field alone does not; SPSR traps)
            if (!(in.operands[0].imm & 0x10) && (in.operands[0].imm & 8)) e.defs = kAllFlags;
            break;
        case Op::Vmrs:
            // vmrs APSR_nzcv, fpscr (Sysreg codes: format.cpp)
            if (in.operands[0].kind == OperandKind::Sysreg && in.operands[0].imm == 11 && in.operands[1].imm == 5) e.defs = kAllFlags;
            break;
        default: break;
    }
    e.kills = e.defs;
    return e;
}

FlagSet FlagLiveness::into(u32 code, FlagSet after) const {
    if (everywhere) return kAllFlags;
    const FunctionFlags* f = at(code);
    if (!f) return into_unknown(after);
    return f->need | (after & f->pass);
}

FlagSet FlagLiveness::at_exit(const FunctionFlags& fn, const Edge& e) const {
    if (everywhere) return e.kind == EdgeKind::Next || e.kind == EdgeKind::Trap ? kNoFlags : kAllFlags;
    switch (e.kind) {
        case EdgeKind::Return: return fn.live_out;
        case EdgeKind::IndirectJump:
        case EdgeKind::ImportTail: return into_unknown(fn.live_out);
        case EdgeKind::TailCall: return into(e.to, fn.live_out);
        default: return kNoFlags;
    }
}

namespace {

// Both solutions in one byte: bits 3-0 with the returns reading no flag, bits 7-4 with them reading all four.
using Packed = u8;
constexpr Packed both(FlagSet f) { return static_cast<Packed>(f | (f << 4)); }
constexpr FlagSet with(Packed p, FlagSet returns) { return static_cast<FlagSet>((p & 15) | ((p >> 4) & returns)); }

struct Fn {
    u32 code = 0;
    const Function* fn = nullptr;
    Cfg cfg;
    nx::vector<FlagEffect> effect;
    nx::vector<u8> inline_trap;   // svc, vmsr: may trap inside the instruction's own translation
    nx::vector<u32> callers;      // functions that call or tail-call this one (indices)
    nx::vector<Packed> live;      // before each instruction
    nx::vector<Packed> after;     // after its effect
    nx::vector<Packed> after_call;
    FlagSet need = 0, pass = 0;   // pass: kAllFlags for a function without code (it changes nothing)
    FlagSet live_out = 0;
    FlagSet live_out_read = 0;
    bool queued = false;
};

class Solver {
public:
    Solver(const Program& program, const FlagOptions& options) : program_(program), options_(options) {}
    FlagLiveness run();

private:
    u32 index(u32 code) const {
        auto it = index_.find(code);
        return it == index_.end() ? kNoIndex : it->second;
    }
    Packed edge(const Fn& f, const Edge& e) const;
    Packed call(const CfgNode& node, Packed after) const;
    void solve(Fn& f);
    void solve_soft(const Fn& f, nx::vector<FlagSet>& soft) const;

    const Program& program_;
    const FlagOptions& options_;
    nx::vector<Fn> fns_;
    nx::flat_hash_map<u32, u32> index_;
    FlagSet given_entry_ = 0;
};

Packed Solver::edge(const Fn& f, const Edge& e) const {
    switch (e.kind) {
        case EdgeKind::Next: return f.live[e.to];
        // A return: what the callers read. (A return-form branch to an address other than the return address is a
        // non-local exit, which passes only these: ARCHITECTURE.md, "Condition flags".)
        case EdgeKind::Return: return 0xF0;
        // Code the module does not know: what it may read at entry, and what reaches this function's returns.
        case EdgeKind::IndirectJump:
        case EdgeKind::ImportTail: return static_cast<Packed>(0xF0 | both(given_entry_));
        case EdgeKind::TailCall: {
            const u32 t = index(e.to);
            if (t == kNoIndex) return static_cast<Packed>(0xF0 | both(given_entry_));
            return static_cast<Packed>(fns_[t].need | (fns_[t].pass << 4));
        }
        default: return 0;
    }
}

Packed Solver::call(const CfgNode& node, Packed after) const {
    if (node.call == CallKind::Direct)
        if (const u32 t = index(node.call_target); t != kNoIndex)
            return static_cast<Packed>(both(fns_[t].need) | (after & both(fns_[t].pass)));
    return static_cast<Packed>(both(given_entry_) | after);
}

void Solver::solve(Fn& f) {
    const usize n = f.cfg.nodes.size();
    f.live.assign(n, 0);
    f.after.assign(n, 0);
    f.after_call.assign(n, 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (usize k = n; k-- > 0;) {
            const CfgNode& node = f.cfg.nodes[k];
            Packed exec = 0;
            for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e) exec |= edge(f, *e);
            if (node.call != CallKind::None) {
                f.after_call[k] = exec;
                exec = call(node, exec);
            }
            f.after[k] = exec;
            Packed b = static_cast<Packed>(both(f.effect[k].uses) | (exec & ~both(f.effect[k].kills)));
            if (node.conditional) b |= edge(f, node.skip);
            if (b != f.live[k]) f.live[k] = b, changed = true;
        }
    }
    if (f.cfg.entry != kNoIndex) {
        f.need = f.live[f.cfg.entry] & 15;
        f.pass = static_cast<FlagSet>(f.live[f.cfg.entry] >> 4);
    }
}

// What a trap may show: the flags at a trap are those of the function's own instructions since its entry or its
// last call (a trap reads every flag, but that does not reach past a call or into the callers).
void Solver::solve_soft(const Fn& f, nx::vector<FlagSet>& soft) const {
    const usize n = f.cfg.nodes.size();
    soft.assign(n, 0);
    const auto at = [&](const Edge& e) -> FlagSet {
        if (e.kind == EdgeKind::Next) return soft[e.to];
        return e.kind == EdgeKind::Trap ? kAllFlags : kNoFlags;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (usize k = n; k-- > 0;) {
            const CfgNode& node = f.cfg.nodes[k];
            FlagSet exec = 0;
            for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e) exec |= at(*e);
            if (node.call != CallKind::None) exec = 0;
            FlagSet b = static_cast<FlagSet>(exec & ~f.effect[k].kills);
            if (f.inline_trap[k]) b = kAllFlags;
            if (node.conditional) b |= at(node.skip);
            if (b != soft[k]) soft[k] = b, changed = true;
        }
    }
}

FlagLiveness Solver::run() {
    FlagLiveness out;
    out.everywhere = options_.everywhere;
    for (const auto& [code, fn] : program_.functions) {
        index_[code] = static_cast<u32>(fns_.size());
        Fn f;
        f.code = code;
        f.fn = &fn;
        fns_.push_back(std::move(f));
    }
    if (options_.everywhere) {
        for (const Fn& f : fns_) {
            FunctionFlags& r = out.functions[f.code];
            const usize n = f.fn->code.size();
            r.need = r.pass = r.live_out = r.live_in = kAllFlags;
            r.after.assign(n, kAllFlags);
            r.after_call.assign(n, kAllFlags);
            r.before_call.assign(n, kAllFlags);
            r.live_before.assign(n, kAllFlags);
        }
        out.needed_at_entry = out.given_at_entry = out.needed_at_return = out.given_at_return = kAllFlags;
        return out;
    }
    for (Fn& f : fns_) {
        f.cfg = build_cfg(program_, *f.fn);
        const usize n = f.fn->code.size();
        f.effect.resize(n);
        f.inline_trap.assign(n, 0);
        for (usize k = 0; k < n; ++k) {
            const arm::Instruction& in = f.fn->code[k];
            f.effect[k] = flag_effect(in);
            f.inline_trap[k] = in.op == Op::Svc || in.op == Op::Vmsr;
        }
        if (n == 0) f.pass = kAllFlags;  // (generated as a function that returns at once, the context untouched)
    }
    // Who calls whom (direct calls and tail calls), to solve a caller again when a callee's summary grows.
    for (u32 i = 0; i < fns_.size(); ++i) {
        const Fn& f = fns_[i];
        nx::flat_hash_set<u32> callees;
        for (const CfgNode& node : f.cfg.nodes) {
            if (node.call == CallKind::Direct) callees.insert(index(node.call_target));
            for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e)
                if (e->kind == EdgeKind::TailCall) callees.insert(index(e->to));
            if (node.conditional && node.skip.kind == EdgeKind::TailCall) callees.insert(index(node.skip.to));
        }
        nx::vector<u32> sorted(callees.begin(), callees.end());
        std::sort(sorted.begin(), sorted.end());
        for (u32 c : sorted)
            if (c != kNoIndex) fns_[c].callers.push_back(i);
    }

    // 1. Each function's need and pass, the least fixed point over the call graph; and the module's need at entry,
    //    which every call to unknown code provides.
    given_entry_ = options_.given_at_entry;
    for (;;) {
        nx::vector<u32> work;
        for (u32 i = 0; i < fns_.size(); ++i) work.push_back(i), fns_[i].queued = true;
        while (!work.empty()) {
            const u32 i = work.back();
            work.pop_back();
            Fn& f = fns_[i];
            f.queued = false;
            const FlagSet need = f.need, pass = f.pass;
            solve(f);
            if (f.need != need || f.pass != pass)
                for (u32 c : f.callers)
                    if (!fns_[c].queued) fns_[c].queued = true, work.push_back(c);
        }
        FlagSet needed = 0;
        for (const Fn& f : fns_) needed |= f.need;
        out.needed_at_entry = needed;
        const FlagSet given = static_cast<FlagSet>(needed | options_.given_at_entry);
        if (given == given_entry_) break;
        given_entry_ = given;
    }
    out.given_at_entry = given_entry_;

    // 2. What each function's returns leave in the context: what its direct callers read after the call, what the
    //    functions that end in it must leave, all four for exports; and the module's flags at every return, what its
    //    calls and jumps to unknown code read after they return. Two tracks: `live_out`, what the returns leave, and
    //    `live_out_read`, what some instruction reads (an export's four flags are the first only: what it passes on
    //    from an indirect call or jump it ends in is that code's, the module's flags at return).
    nx::flat_hash_set<u32> full_exit(options_.full_exit.begin(), options_.full_exit.end());
    if (program_.image)
        for (const auto& lib : program_.image->module().exports)
            if (!(lib.attributes & module::kExportMain))
                for (const auto& f : lib.functions) full_exit.insert(f.address);
    FlagSet given_return = options_.given_at_return, needed_return = 0;
    for (;;) {
        nx::vector<u32> work;
        for (u32 i = 0; i < fns_.size(); ++i) {
            Fn& f = fns_[i];
            f.live_out |= given_return;
            f.live_out_read |= given_return;
            if (full_exit.contains(f.code)) f.live_out = kAllFlags;
            work.push_back(i);
            f.queued = true;
        }
        const auto raise = [&](u32 t, FlagSet flags, FlagSet read) {
            if (t == kNoIndex) return;
            Fn& g = fns_[t];
            if ((g.live_out | flags) == g.live_out && (g.live_out_read | read) == g.live_out_read) return;
            g.live_out |= flags;
            g.live_out_read |= read;
            if (!g.queued) g.queued = true, work.push_back(t);
        };
        while (!work.empty()) {
            const u32 i = work.back();
            work.pop_back();
            Fn& f = fns_[i];
            f.queued = false;
            for (usize k = 0; k < f.cfg.nodes.size(); ++k) {
                const CfgNode& node = f.cfg.nodes[k];
                const FlagSet a = with(f.after_call[k], f.live_out), a_read = with(f.after_call[k], f.live_out_read);
                if (node.call == CallKind::Direct) raise(index(node.call_target), a, a_read);
                else if (node.call == CallKind::Indirect || node.call == CallKind::System) needed_return |= a_read;
                const auto exit = [&](const Edge& e) {
                    if (e.kind == EdgeKind::TailCall) raise(index(e.to), f.live_out, f.live_out_read);
                    else if (e.kind == EdgeKind::IndirectJump) needed_return |= f.live_out_read;
                };
                for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e) exit(*e);
                if (node.conditional) exit(node.skip);
            }
        }
        const FlagSet given = static_cast<FlagSet>(needed_return | options_.given_at_return);
        if (given == given_return) break;
        given_return = given;
    }
    out.needed_at_return = needed_return;
    out.given_at_return = given_return;

    // 3. The sets the generator uses, with each function's returns reading its live_out; and what a trap may show.
    nx::vector<FlagSet> soft;
    for (const Fn& f : fns_) {
        FunctionFlags& r = out.functions[f.code];
        r.need = f.need;
        r.pass = f.pass;
        r.live_out = f.live_out;
        const usize n = f.cfg.nodes.size();
        solve_soft(f, soft);
        r.after.resize(n);
        r.after_call.assign(n, 0);
        r.before_call.assign(n, 0);
        r.live_before.resize(n);
        for (usize k = 0; k < n; ++k) {
            const CfgNode& node = f.cfg.nodes[k];
            r.live_before[k] = with(f.live[k], f.live_out) | soft[k];
            FlagSet trap_after = 0;
            if (node.call == CallKind::None) {
                for (const Edge* e = f.cfg.begin(node); e != f.cfg.end(node); ++e)
                    trap_after |= e->kind == EdgeKind::Next ? soft[e->to] : e->kind == EdgeKind::Trap ? kAllFlags : kNoFlags;
            }
            r.after[k] = with(f.after[k], f.live_out) | trap_after;
            if (node.call != CallKind::None) {
                const FlagSet a = with(f.after_call[k], f.live_out);
                r.after_call[k] = a;
                const u32 t = node.call == CallKind::Direct ? index(node.call_target) : kNoIndex;
                r.before_call[k] = t != kNoIndex ? static_cast<FlagSet>(fns_[t].need | (a & fns_[t].pass)) : out.into_unknown(a);
            }
        }
        if (f.cfg.entry != kNoIndex) r.live_in = with(f.live[f.cfg.entry], f.live_out) | soft[f.cfg.entry];
    }
    return out;
}

}  // namespace

FlagLiveness flag_liveness(const Program& program, const FlagOptions& options) { return Solver(program, options).run(); }

}  // namespace psvrecore::analysis
