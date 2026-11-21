#include "psvrecore/recomp/generator.hpp"

#include <algorithm>
#include <bit>
#include <utility>

#include "emit.hpp"
#include "psvrecore/arm/format.hpp"

namespace psvrecore::recomp {

using namespace detail;

nx::string function_name(u32 code) { return nx::format("f_{:08X}", code & ~1u); }
nx::string internal_function_name(u32 code) { return nx::format("fi_{:08X}", code & ~1u); }
nx::string guest_code_name(u32 code) { return nx::format("fo_{:08X}", code & ~1u); }

namespace detail {

nx::string condition(arm::Cond c) {
    switch (c) {
        case arm::Cond::Eq: return "fl.z";
        case arm::Cond::Ne: return "!fl.z";
        case arm::Cond::Cs: return "fl.c";
        case arm::Cond::Cc: return "!fl.c";
        case arm::Cond::Mi: return "fl.n";
        case arm::Cond::Pl: return "!fl.n";
        case arm::Cond::Vs: return "fl.v";
        case arm::Cond::Vc: return "!fl.v";
        case arm::Cond::Hi: return "(fl.c & !fl.z)";
        case arm::Cond::Ls: return "(!fl.c | fl.z)";
        case arm::Cond::Ge: return "fl.n == fl.v";
        case arm::Cond::Lt: return "fl.n != fl.v";
        case arm::Cond::Gt: return "(!fl.z & (fl.n == fl.v))";
        case arm::Cond::Le: return "(fl.z | (fl.n != fl.v))";
        default: return "";
    }
}

namespace {

// Text safe inside a C comment.
nx::string comment_text(nx::string_view text) {
    nx::string out;
    for (usize i = 0; i < text.size(); ++i) {
        out += text[i];
        if (text[i] == '*' && i + 1 < text.size() && text[i + 1] == '/') out += ' ';
    }
    return out;
}

constexpr std::pair<FlagSet, char> kFlagNames[] = {{analysis::kFlagN, 'n'}, {analysis::kFlagZ, 'z'}, {analysis::kFlagC, 'c'}, {analysis::kFlagV, 'v'}};

// What the function's own code reads, after the call at instruction `index`, of r1-r3, r12, lr and the flags before
// writing them: what it would take from a callee that the AAPCS lets leave them changed (a replaced function's guest
// code; Options::replacements). The paths from the call's return through the function's instructions, each ending at
// the next call (whose callee may change them), return, jump or trap: what is live there is the AAPCS's business of
// that code, not a read of this function (the register and flag liveness count it, the context being every function's
// at its exits, so they cannot tell this).
void reads_after_call(const analysis::Function& fn, const analysis::Cfg& cfg, u32 index, RegSet& regs, FlagSet& flags) {
    constexpr RegSet kScratch = static_cast<RegSet>((1u << 1) | (1u << 2) | (1u << 3) | (1u << 12) | (1u << 14));
    regs = 0;
    flags = 0;
    nx::vector<RegSet> seen_regs(fn.code.size(), 0);
    nx::vector<FlagSet> seen_flags(fn.code.size(), 0);
    nx::vector<std::tuple<u32, RegSet, FlagSet>> work;
    const auto go = [&](const analysis::Edge& e, RegSet r, FlagSet f) {
        if (e.kind != analysis::EdgeKind::Next || e.to >= fn.code.size()) return;
        const RegSet nr = static_cast<RegSet>(r & ~seen_regs[e.to]);
        const FlagSet nf = static_cast<FlagSet>(f & ~seen_flags[e.to]);
        if (!nr && !nf) return;
        seen_regs[e.to] |= nr;
        seen_flags[e.to] |= nf;
        work.emplace_back(e.to, nr, nf);
    };
    const analysis::CfgNode& at = cfg.nodes[index];
    for (const analysis::Edge* e = cfg.begin(at); e != cfg.end(at); ++e) go(*e, kScratch, analysis::kAllFlags);
    while (!work.empty()) {
        const auto [i, r, f] = work.back();
        work.pop_back();
        const analysis::CfgNode& n = cfg.nodes[i];
        const analysis::RegisterEffect re = analysis::register_effect(fn.code[i]);
        const analysis::FlagEffect fe = analysis::flag_effect(fn.code[i]);
        regs |= static_cast<RegSet>(re.uses & r);
        flags |= static_cast<FlagSet>(fe.uses & f);
        if (n.conditional) go(n.skip, r, f);
        if (n.call != analysis::CallKind::None) continue;
        for (const analysis::Edge* e = cfg.begin(n); e != cfg.end(n); ++e)
            go(*e, static_cast<RegSet>(r & ~re.defs), static_cast<FlagSet>(f & ~fe.kills));
    }
}

}  // namespace

Emitter::Emitter(const analysis::Program& program, const analysis::Function& fn, const analysis::FlagLiveness& flags,
                 const nx::string& prefix, Stats& stats, RegisterMode mode, const analysis::FunctionRegisters* registers,
                 VfpMode vfp_mode, const analysis::FunctionVfp* vfp)
    : program_(program), fn_(fn), flags_(flags), flags_fn_(flags.at(fn.entry)), cfg_(analysis::build_cfg(program, fn)),
      prefix_(prefix), stats_(stats), mode_(mode), regs_(mode == RegisterMode::Locals ? registers : nullptr), vfp_mode_(vfp_mode),
      vfp_(vfp_mode == VfpMode::Locals ? vfp : nullptr) {
    if (mode == RegisterMode::Locals && !registers) mode_ = RegisterMode::Context;
    if (vfp_mode == VfpMode::Locals && (!vfp || vfp->live_before.size() != fn.code.size())) vfp_mode_ = VfpMode::Context, vfp_ = nullptr;
}

namespace {

constexpr RegSet reg_bit(u32 r) { return r < 15 ? static_cast<RegSet>(1u << r) : RegSet(0); }

// An internal entry's C arguments, in this order (analysis::kArgumentRegisters), and its result type.
constexpr u32 kParamOrder[] = {0, 1, 2, 3, 13, 14};
const char* result_type(RegSet results) { return results == 3 ? "uint64_t" : results ? "uint32_t" : "void"; }

}  // namespace

nx::string internal_signature(u32 code, const analysis::FunctionRegisters& r) {
    nx::string s = nx::format("{} {}(PsvContext* PSV_RESTRICT ctx", result_type(r.results), internal_function_name(code));
    for (u32 k : kParamOrder)
        if (r.params & (1u << k)) s += nx::format(", uint32_t g{}", k);
    return s + ")";
}

// A call of it with each argument the register's `prefix`<k> ("g": the caller's locals; "ctx->r[" ... "]").
nx::string internal_call(u32 code, const analysis::FunctionRegisters& r, nx::string_view prefix, nx::string_view suffix) {
    nx::string s = nx::format("{}(ctx", internal_function_name(code));
    for (u32 k : kParamOrder)
        if (r.params & (1u << k)) s += nx::format(", {}{}{}", prefix, k, suffix);
    return s + ")";
}

nx::string Emitter::internal_return() const {
    const RegSet res = regs_ ? regs_->results : RegSet(0);
    if (res == 3) return "return (uint64_t)g0 | ((uint64_t)g1 << 32);";
    if (res == 1) return "return g0;";
    if (res == 2) return "return g1;";
    return "return;";
}

nx::string Emitter::register_line(RegSet r, bool store) const {
    nx::string out;
    for (u32 k = 0; k < 15; ++k) {
        if (!(r & (1u << k))) continue;
        if (!out.empty()) out += ' ';
        out += store ? nx::format("ctx->r[{}] = g{};", k, k) : nx::format("g{} = ctx->r[{}];", k, k);
    }
    return out;
}

void Emitter::store_registers(RegSet r) {
    if (mode_ != RegisterMode::Locals || !r) return;
    line(register_line(r, true));
    stats_.register_stores += static_cast<usize>(std::popcount(r));
}

void Emitter::load_registers(RegSet r) {
    if (mode_ != RegisterMode::Locals || !r) return;
    line(register_line(r, false));
    stats_.register_call_loads += static_cast<usize>(std::popcount(r));
}

bool Emitter::noreturn_call(const Instruction& in) const {
    return std::find(fn_.noreturn_calls.begin(), fn_.noreturn_calls.end(), in.address) != fn_.noreturn_calls.end();
}

// ---- Floating-point registers in locals (analysis/vfp.hpp) -------------------------------------------------------

nx::string Emitter::vs(u32 k) {
    k &= 63;
    vaccess_.uses.words |= 1ull << k;
    if (k < 32) vaccess_.s_access |= 1u << (k / 2);
    const u32 n = k / 2;
    if (vfp_ && vfp_->q_held_d(n)) return nx::format("psv_vqf(q{}, {})", n / 2, k % 4);
    if (!vfp_ || vfp_->s_held(n)) return nx::format("s{}", k);
    return nx::format("psv_v{}32(d{})", (k & 1) ? "hi" : "lo", n);
}

nx::string Emitter::vd(u32 n) {
    n &= 31;
    vaccess_.uses.words |= analysis::d_words(n);
    vaccess_.d_access |= 1u << n;
    if (vfp_ && vfp_->q_held_d(n)) return nx::format("psv_vd(psv_q{}(q{}))", (n & 1) ? "hi" : "lo", n / 2);
    if (vfp_ && vfp_->s_held(n)) return nx::format("psv_vpack64(s{}, s{})", 2 * n, 2 * n + 1);
    return nx::format("d{}", n);
}

void Emitter::set_vs(u32 k, nx::string_view value) {
    k &= 63;
    vaccess_.defs.words |= 1ull << k;
    if (k < 32) vaccess_.s_access |= 1u << (k / 2);
    const u32 n = k / 2;
    if (vfp_ && vfp_->q_held_d(n)) line(nx::format("q{0} = psv_vqsetf(q{0}, {1}, {2});", n / 2, k % 4, value));
    else if (!vfp_ || vfp_->s_held(n)) line(nx::format("s{} = {};", k, value));
    else line(nx::format("d{0} = psv_vset{1}64(d{0}, {2});", n, (k & 1) ? "hi" : "lo", value));
}

void Emitter::set_vd(u32 n, nx::string_view value) {
    n &= 31;
    vaccess_.defs.words |= analysis::d_words(n);
    vaccess_.d_access |= 1u << n;
    if (vfp_ && vfp_->q_held_d(n)) {
        line(nx::format("q{0} = psv_qset{1}(q{0}, psv_dv({2}));", n / 2, (n & 1) ? "hi" : "lo", value));
    } else if (vfp_ && vfp_->s_held(n)) {
        const nx::string t = temp("v");
        line(nx::format("const double {0} = {1}; s{2} = psv_vlo32({0}); s{3} = psv_vhi32({0});", t, value, 2 * n, 2 * n + 1));
    } else {
        line(nx::format("d{} = {};", n, value));
    }
}

// A D register's local as a D value (PsvD), whatever holds it (no access recorded).
nx::string Emitter::dv_of(u32 n) const {
    if (vfp_ && vfp_->q_held_d(n)) return nx::format("psv_q{}(q{})", (n & 1) ? "hi" : "lo", n / 2);
    if (vfp_ && vfp_->s_held(n)) return nx::format("psv_dvf(s{}, s{})", 2 * n, 2 * n + 1);
    return nx::format("psv_dv(d{})", n);
}

// Writes a D register's local from a D value expression (evaluated once; no access recorded).
void Emitter::dv_assign(u32 n, nx::string_view value) {
    if (vfp_ && vfp_->q_held_d(n)) {
        line(nx::format("q{0} = psv_qset{1}(q{0}, {2});", n / 2, (n & 1) ? "hi" : "lo", value));
    } else if (vfp_ && vfp_->s_held(n)) {
        const nx::string t = temp("v");
        line(nx::format("const PsvD {0} = {1}; s{2} = psv_vdf({0}, 0); s{3} = psv_vdf({0}, 1);", t, value, 2 * n, 2 * n + 1));
    } else {
        line(nx::format("d{} = psv_vd({});", n, value));
    }
}

nx::string Emitter::vdv(u32 n) {
    n &= 31;
    vaccess_.uses.words |= analysis::d_words(n);
    vaccess_.d_access |= 1u << n;
    return dv_of(n);
}

void Emitter::set_vdv(u32 n, nx::string_view value) {
    n &= 31;
    vaccess_.defs.words |= analysis::d_words(n);
    vaccess_.d_access |= 1u << n;
    dv_assign(n, value);
}

nx::string Emitter::vq(u32 p) {
    p &= 15;
    vaccess_.uses.words |= analysis::q_words(p);
    vaccess_.q_access |= 1u << p;
    if (vfp_ && vfp_->q_held_d(2 * p)) return nx::format("q{}", p);
    return nx::format("psv_qcat({}, {})", dv_of(2 * p), dv_of(2 * p + 1));
}

void Emitter::set_vq(u32 p, nx::string_view value) {
    p &= 15;
    vaccess_.defs.words |= analysis::q_words(p);
    vaccess_.q_access |= 1u << p;
    if (vfp_ && vfp_->q_held_d(2 * p)) {
        line(nx::format("q{} = {};", p, value));
        return;
    }
    const nx::string t = temp("qv");
    line(nx::format("const PsvQ {} = {};", t, value));
    dv_assign(2 * p, nx::format("psv_qlo({})", t));
    dv_assign(2 * p + 1, nx::format("psv_qhi({})", t));
}

nx::string Emitter::fpscr_qc() {
    vaccess_.uses.fpscr |= analysis::kFpscrRest;
    vaccess_.defs.fpscr |= analysis::kFpscrRest;
    return "&fpscr";
}

nx::string Emitter::fpscr() {
    vaccess_.uses.fpscr |= analysis::kFpscrRest;
    return "fpscr";
}

nx::string Emitter::fpnzcv() {
    vaccess_.uses.fpscr |= analysis::kFpscrNzcv;
    return "fpnzcv";
}

void Emitter::set_fpnzcv(nx::string_view value) {
    vaccess_.defs.fpscr |= analysis::kFpscrNzcv;
    line(nx::format("fpnzcv = {};", value));
}

void Emitter::set_fpscr(nx::string_view value) {
    vaccess_.defs.fpscr |= analysis::kFpscrAll;
    line(nx::format("fpscr = {}; fpnzcv = fpscr & 0xF0000000u;", value));
}

nx::string Emitter::vfp_line(VfpSet units, bool store) const {
    nx::string out;
    const auto add = [&](nx::string_view s) {
        if (!out.empty()) out += ' ';
        out += s;
    };
    for (u32 n = 0; n < 32; ++n) {
        if (vfp_ && vfp_->q_held_d(n)) {
            // (a Q-held register's unit is all four words: in both D registers' places at once)
            if ((n & 1) == 0 && (units.words & analysis::q_words(n / 2)))
                add(store ? nx::format("psv_ctx_qput(ctx, {}, q{});", n, n / 2) : nx::format("q{} = psv_ctx_qget(ctx, {});", n / 2, n));
        } else if (vfp_ && vfp_->s_held(n)) {
            for (u32 k = 2 * n; k <= 2 * n + 1; ++k)
                if ((units.words >> k) & 1)
                    add(store ? nx::format("ctx->vfp.s[{0}] = psv_b32(s{0});", k) : nx::format("s{0} = psv_f32(ctx->vfp.s[{0}]);", k));
        } else if (units.words & analysis::d_words(n)) {
            add(store ? nx::format("ctx->vfp.d[{0}] = psv_b64(d{0});", n) : nx::format("d{0} = psv_f64(ctx->vfp.d[{0}]);", n));
        }
    }
    const u8 f = units.fpscr;
    if (store) {
        if (f == analysis::kFpscrAll) add("ctx->fpscr = (fpscr & 0x0FFFFFFFu) | fpnzcv;");
        else if (f == analysis::kFpscrNzcv) add("ctx->fpscr = (ctx->fpscr & 0x0FFFFFFFu) | fpnzcv;");
        else if (f == analysis::kFpscrRest) add("ctx->fpscr = (ctx->fpscr & 0xF0000000u) | (fpscr & 0x0FFFFFFFu);");
    } else {
        if (f & analysis::kFpscrRest) add("fpscr = ctx->fpscr;");
        if (f & analysis::kFpscrNzcv) add("fpnzcv = ctx->fpscr & 0xF0000000u;");
    }
    return out;
}

void Emitter::store_vfp(VfpSet units, usize& counter) {
    if (!vfp_ || units.empty()) return;
    line(vfp_line(units, true));
    ++counter;
}

void Emitter::call_vfp_before() {
    if (!vfp_) return;
    store_vfp(vfp_->call_store[index_], stats_.vfp_stores);
}

void Emitter::call_vfp_after(const Instruction& in) {
    if (!vfp_ || noreturn_call(in)) return;
    const VfpSet load = vfp_->call_load[index_];
    if (load.empty()) return;
    line(vfp_line(load, false));
    ++stats_.vfp_call_loads;
}

nx::string Emitter::vfp_trap_stores() {
    vaccess_.traps = true;
    if (!vfp_ || vfp_->trap_store[index_].empty()) return {};
    ++stats_.vfp_trap_stores;
    return vfp_line(vfp_->trap_store[index_], true);
}

namespace {

// The condition `c` after a fused compare of fc<w>a with fc<w>b: NZCV is less 1000, equal 0110, greater 0010,
// unordered 0011, so each condition is one comparison (an ordered one, or the negation of one).
nx::string fused_condition(arm::Cond c, u32 width) {
    const nx::string a = nx::format("fc{}a", width), b = nx::format("fc{}b", width);
    switch (c) {
        case arm::Cond::Eq: return nx::format("({} == {})", a, b);
        case arm::Cond::Ne: return nx::format("!({} == {})", a, b);
        case arm::Cond::Cs: return nx::format("!({} < {})", a, b);
        case arm::Cond::Cc: return nx::format("({} < {})", a, b);
        case arm::Cond::Mi: return nx::format("({} < {})", a, b);
        case arm::Cond::Pl: return nx::format("!({} < {})", a, b);
        case arm::Cond::Vs: return nx::format("__builtin_isunordered({}, {})", a, b);
        case arm::Cond::Vc: return nx::format("!__builtin_isunordered({}, {})", a, b);
        case arm::Cond::Hi: return nx::format("!({} <= {})", a, b);
        case arm::Cond::Ls: return nx::format("({} <= {})", a, b);
        case arm::Cond::Ge: return nx::format("({} >= {})", a, b);
        case arm::Cond::Lt: return nx::format("!({} >= {})", a, b);
        case arm::Cond::Gt: return nx::format("({} > {})", a, b);
        case arm::Cond::Le: return nx::format("!({} > {})", a, b);
        default: return {};
    }
}

bool is_vfp_compare(const Instruction& in) {
    if (in.op != Op::Vcmp && in.op != Op::Vcmpe) return false;
    if (in.dt == arm::Dt::F64) return true;
    return in.operand_count >= 1 && in.operands[0].kind == OperandKind::SReg;
}

bool is_vmrs_nzcv(const Instruction& in) {
    // (Sysreg codes: format.cpp; 5 FPSCR, 11 APSR_nzcv)
    return in.op == Op::Vmrs && in.operand_count == 2 && in.operands[0].kind == OperandKind::Sysreg && in.operands[0].imm == 11 &&
           in.operands[1].imm == 5;
}

}  // namespace

// A vcmp right before vmrs APSR_nzcv in the same block (both unconditional): the compare keeps its operands, the vmrs
// computes only the flags live after the instructions that follow it and test only its flags in their conditions,
// and those conditions compare the operands themselves (cmp + branch: one host compare and branch).
void Emitter::plan_fused_compares() {
    const auto& code = fn_.code;
    const usize n = code.size();
    fuse_.assign(n, 0);
    fused_flags_.assign(n, 0);
    fused_cond_.assign(n, 0);
    const auto same_block = [&](usize i, usize j) {
        return cfg_.block_of[i] == cfg_.block_of[j] && code[j].address == code[j - 1].address + code[j - 1].size &&
               !labels_.contains(code[j].address);
    };
    for (usize i = 0; i + 1 < n; ++i) {
        const Instruction& c = code[i];
        if (!is_vfp_compare(c) || analysis::is_conditional(c) || !c.valid() || (c.flags & arm::kUnpredictable)) continue;
        // (the vmrs: the next instruction, or a later one of the block, after instructions that leave the FPSCR's NZCV
        // alone and call nothing)
        usize m = i + 1;
        while (m < n && same_block(i, m) && !is_vmrs_nzcv(code[m])) {
            const Instruction& x = code[m];
            if (is_vfp_compare(x) || x.op == Op::Vmsr || x.op == Op::Vmrs || cfg_.nodes[m].call != analysis::CallKind::None || !x.valid() ||
                (x.flags & arm::kUnpredictable))
                break;
            ++m;
        }
        if (m >= n || !same_block(i, m) || !is_vmrs_nzcv(code[m]) || analysis::is_conditional(code[m])) continue;
        const u8 width = c.dt == arm::Dt::F64 ? 64 : 32;
        fuse_[i] = width;
        fuse_[m] = static_cast<u8>(0x80 | width);
        usize last = m;
        for (usize j = m + 1; j < n && same_block(i, j); ++j) {
            const Instruction& in = code[j];
            const analysis::FlagEffect fe = analysis::flag_effect(in);
            const bool conditional = analysis::is_conditional(in) && in.op != Op::It;
            const FlagSet tested = conditional ? analysis::condition_flags(in.cond) : FlagSet(0);
            if (fe.defs || (fe.uses & ~tested) || is_vfp_compare(in) || !in.valid() || (in.flags & arm::kUnpredictable)) break;
            if (vfp_ && vfp_->may_trap[j]) break;
            const bool call = cfg_.nodes[j].call != analysis::CallKind::None;
            if (call && !conditional) break;
            if (conditional) fused_cond_[j] = width;
            last = j;
            if (call) break;  // (the flags are the callee's after it)
        }
        // (the flags something reads after the window: on each way out of it, the skip edge of its last
        // instruction included; a trap there shows all four)
        FlagSet needed = 0;
        const auto edge_flags = [&](const analysis::Edge& e) -> FlagSet {
            switch (e.kind) {
                case analysis::EdgeKind::Next: return e.to > m && e.to <= last ? FlagSet(0) : flags_fn_->live_before[e.to];
                case analysis::EdgeKind::Trap: return analysis::kAllFlags;
                default: return flags_.at_exit(*flags_fn_, e);
            }
        };
        for (usize j = m; j <= last; ++j) {
            const analysis::CfgNode& node = cfg_.nodes[j];
            if (node.call != analysis::CallKind::None) needed |= flags_fn_->before_call[j];  // (a conditional call's)
            for (const analysis::Edge* e = cfg_.begin(node); e != cfg_.end(node); ++e) needed |= edge_flags(*e);
            if (node.conditional) needed |= edge_flags(node.skip);
        }
        fused_flags_[m] = needed;
    }
}

void Emitter::exit_registers() {
    store_registers(regs_ ? regs_->exit_store[index_] : RegSet(0));
    if (vfp_) store_vfp(vfp_->exit_store[index_], stats_.vfp_stores);
}

void Emitter::call_registers_before() {
    call_vfp_before();
    if (!regs_) return;
    store_registers(regs_->call_store[index_]);
}

void Emitter::call_registers_after(const Instruction& in) {
    call_vfp_after(in);
    if (!regs_ || noreturn_call(in)) return;
    const auto& node = cfg_.nodes[index_];
    RegSet kept = regs_->call_kept[index_];
    // (after a call of an Inside internal entry the context's r0-r3, sp and lr are not the callee's to make exact)
    if (internal_body() || (all_ && all_->internal_calls))
        if (!regs_->call_entry.empty() && regs_->call_entry[index_] == static_cast<u8>(analysis::EntryKind::Inside))
            kept &= static_cast<RegSet>(~analysis::kArgumentRegisters);
    if (node.call == analysis::CallKind::Direct) {
        if (kept) {
            // (the callee preserves them: builds with PSV_CHECK_KEPT_REGISTERS check that it did)
            nx::string same;
            for (u32 k = 0; k < 15; ++k)
                if (kept & (1u << k)) same += nx::format("{}ctx->r[{}] == g{}", same.empty() ? "" : " && ", k, k);
            line(nx::format("PSV_KEPT(ctx, {}, {});", hex(in.address), same));
            ++stats_.calls_keeping;
            stats_.registers_kept += static_cast<usize>(std::popcount(kept));
        }
    } else if (kept & analysis::kSpRegister) {
        // (code outside the module's view returns with the stack pointer it was called with: checked, as what the
        // function's callers know it preserves relies on it; one trap for all of the function's checks, at its end,
        // the report naming the function)
        line("if (__builtin_expect(ctx->r[13] != g13, 0)) goto L_SP_MOVED;");
        sp_trap_ = true;
        ++stats_.stack_checks;
    }
    load_registers(regs_->call_load[index_]);
}

nx::string Emitter::trap_register_stores() {
    access_.traps = true;
    nx::string vfp = vfp_trap_stores();
    if (!regs_) return vfp;
    const RegSet r = static_cast<RegSet>(regs_->trap_store[index_] | access_.defs);
    if (!r) return vfp;
    stats_.register_trap_stores += static_cast<usize>(std::popcount(r));
    nx::string out = register_line(r, true);
    if (!vfp.empty()) out += " " + vfp;
    return out;
}

nx::string Emitter::trap_stores() {
    nx::string s = flags_line(analysis::kAllFlags, true);
    const nx::string r = trap_register_stores();
    if (!r.empty()) s += (s.empty() ? "" : " ") + r;
    return s;
}

void Emitter::store_for_trap() { line(trap_stores()); }

nx::string Emitter::flags_line(FlagSet f, bool store) const {
    nx::string out;
    for (const auto& [bit, name] : kFlagNames) {
        if (!(f & bit)) continue;
        if (!out.empty()) out += ' ';
        out += store ? nx::format("ctx->{} = fl.{};", name, name) : nx::format("fl.{} = ctx->{};", name, name);
    }
    return out;
}

void Emitter::store_flags(FlagSet f) {
    if (f) line(flags_line(f, true));
}

void Emitter::load_flags(FlagSet f) {
    if (f) line(flags_line(f, false));
}

void Emitter::set_nz(nx::string_view r, FlagSet which) {
    const FlagSet f = which & live();
    if (f & analysis::kFlagN) line(nx::format("fl.n = {} >> 31;", r));
    if (f & analysis::kFlagZ) line(nx::format("fl.z = {} == 0;", r));
}

void Emitter::add_with_carry(nx::string_view r, nx::string_view x, nx::string_view y, nx::string_view carry_in, FlagSet which) {
    const FlagSet f = which & live();
    if (f & analysis::kFlagC) {
        const nx::string w = temp("w");
        line(nx::format("const uint64_t {} = (uint64_t){} + {} + {};", w, x, y, carry_in));
        line(nx::format("const uint32_t {} = (uint32_t){};", r, w));
        line(nx::format("fl.c = (uint32_t)({} >> 32);", w));
    } else {
        line(nx::format("const uint32_t {} = {} + {} + {};", r, x, y, carry_in));
    }
    set_nz(r, f);
    if (f & analysis::kFlagV) line(nx::format("fl.v = (({} ^ {}) & ({} ^ {})) >> 31;", x, r, y, r));
}

void Emitter::trap(const Instruction& in, const char* kind, nx::string_view what) {
    store_for_trap();
    line(nx::format("psv_trap(ctx, {}, {});", hex(in.address), kind));
    ++stats_.traps[nx::string(what)];
    ended_ = true;
}

void Emitter::unimplemented(const Instruction& in) {
    trap(in, "PSV_TRAP_UNIMPLEMENTED", nx::format("unimplemented {}", arm::mnemonic(in.op)));
}

nx::string Emitter::variable_address(u32 site, u8& type) const {
    const auto it = program_.variable_uses.find(site);
    if (it == program_.variable_uses.end()) return {};
    type = it->second.type;
    return nx::format("({}_variables[{}].address + {})", prefix_, it->second.variable, hex(it->second.addend));
}

nx::string Emitter::read(const Instruction& in, u32 r) {
    if (r == arm::kPc) return hex(analysis::pc_value(in));
    return reg(r);
}

nx::string Emitter::reg(u32 r) {
    access_.uses |= reg_bit(r);
    return mode_ == RegisterMode::Context ? nx::format("ctx->r[{}]", r) : nx::format("g{}", r);
}

nx::string Emitter::assignment(u32 r, nx::string_view value) {
    access_.defs |= reg_bit(r);
    // (a base write-back to the PC, which is unpredictable, goes to the context's unused r[15] as it always has)
    return mode_ == RegisterMode::Context || r >= 15 ? nx::format("ctx->r[{}] = {};", r, value) : nx::format("g{} = {};", r, value);
}

void Emitter::write(const Instruction& in, u32 r, nx::string_view value, PcWrite kind) {
    if (r != arm::kPc) {
        line(assignment(r, value));
        return;
    }
    // BranchWritePC (Thumb data processing): bit 0 is ignored and the state stays Thumb. Elsewhere (loads, ARM
    // data processing, bx) bit 0 selects the instruction set.
    if (kind == PcWrite::Branch && in.mode == arm::Mode::Thumb) branch_to_value(in, nx::format("(({}) | 1u)", value));
    else branch_to_value(in, value);
}

void Emitter::table_branch(const Instruction& in, const analysis::JumpTable& table, nx::string_view target) {
    ++stats_.jump_tables;
    const auto& targets = table.targets;
    const u32 n = static_cast<u32>(targets.size());
    if (n == 0) {  // (a bound of no entries: every index is outside the table)
        store_for_trap();
        line(nx::format("psv_trap(ctx, {}, PSV_TRAP_BAD_JUMP);", hex(in.address)));
        return;
    }
    // A switch on the index. Each entry the table branch reads (tbb, tbh, ldr pc) must be in read-only memory the
    // loader does not patch, so that it holds at run time what it holds now; and with the table's address in a
    // register (ldr pc, [rB, rI, lsl #2]), the register must hold that address.
    u32 entry_size = 0;
    if (in.op == Op::Tbb) entry_size = 1;
    else if (in.op == Op::Tbh) entry_size = 2;
    else if (in.op == Op::Ldr) entry_size = 4;
    bool constant = true;
    for (u32 i = 0; entry_size && constant && i < n; ++i) {
        u32 value;
        constant = constant_load(table.table + i * entry_size, entry_size, value);
    }
    const nx::string index = temp("index");
    if (!constant) line(nx::format("uint32_t {} = 0xFFFFFFFFu;", index));
    else if (table.base_reg != 0xFF)
        line(nx::format("uint32_t {} = {} == {} ? {} : 0xFFFFFFFFu;", index, reg(table.base_reg), hex(table.table), read(in, table.index_reg)));
    else line(nx::format("uint32_t {} = {};", index, read(in, table.index_reg)));
    // Any other index (or table): the target the instruction computes, if it is one of the table's (the entry of the
    // first index that has it), as the guest's branch would go there; else a trap.
    const nx::string list = temp("targets");
    line(nx::format("if ({} >= {}u) {{", index, n));
    nx::string values;
    for (u32 i = 0; i < n; ++i) values += (i % 8 == 0 ? "\n                " : " ") + hex(targets[i]) + ",";
    line(nx::format("    static const uint32_t {}[{}] = {{{}\n            }};", list, n, values));
    line(nx::format("    {} = psv_table_find({}, {}u, {});", index, list, n, target));
    line(nx::format("    if ({} >= {}u) {{", index, n));
    line("        " + trap_stores());
    line(nx::format("        psv_trap(ctx, {}, PSV_TRAP_BAD_JUMP);", hex(in.address)));
    line("    }");
    line("}");
    line(nx::format("switch ({}) {{", index));
    for (u32 i = 0; i < n; ++i) {
        const u32 target_code = targets[i];
        const bool tail = target_code != fn_.entry && program_.function_at(target_code) != nullptr;
        if (tail) {
            nx::string store = flags_line(flags_.into(target_code, flags_fn_->live_out), true);
            if (regs_ && regs_->exit_store[index_]) {
                store += (store.empty() ? "" : " ") + register_line(regs_->exit_store[index_], true);
                stats_.register_stores += static_cast<usize>(std::popcount(regs_->exit_store[index_]));
            }
            if (vfp_ && !vfp_->exit_store[index_].empty()) {
                store += (store.empty() ? "" : " ") + vfp_line(vfp_->exit_store[index_], true);
                ++stats_.vfp_stores;
            }
            // (a replaced function's case: its host implementation)
            const nx::string* host = replacement(target_code);
            if (host) ++stats_.replaced_tails;
            line(nx::format("case {}u: {}{}{}(ctx, ret); return;", i, store, store.empty() ? "" : " ", host ? *host : function_name(target_code)));
        } else {
            line(nx::format("case {}u: goto {};", i, label_name(target_code)));
        }
    }
    line("default: __builtin_unreachable();");  // (the index is below n here)
    line("}");
}

void Emitter::branch_to_value(const Instruction& in, nx::string_view target) {
    if (auto table = fn_.tables.find(in.address); table != fn_.tables.end()) return table_branch(in, table->second, target);
    const nx::string t = temp("target");
    line(nx::format("const uint32_t {} = {};", t, target));
    ++stats_.indirect_jumps;
    // A return, with the flags the callers read; else a jump through the runtime: a return-form branch passes only
    // those (a non-local exit), another the flags of the function it goes to when that is a constant, else what code
    // the module does not know reads at entry (analysis/cfg.hpp: Return, IndirectJump).
    const FlagSet out = flags_fn_->live_out;
    FlagSet jump = out;
    const auto& node = cfg_.nodes[index_];
    for (const auto* e = cfg_.begin(node); e != cfg_.end(node); ++e) jump |= flags_.at_exit(*flags_fn_, *e);
    ++stats_.return_sites;
    if (out) ++stats_.return_sites_store;
    if (internal_body()) {
        // An internal entry's every way out goes back to the LR it was entered with (analysis::EntryKind): a return to
        // its caller, with no compare. The flags a jump through the runtime would leave are left (its wrapper makes
        // that jump when entered with an LR other than ret).
        store_flags(jump);
        exit_registers();
        ++stats_.internal_returns;
        line(nx::format("(void){};", t));
        line(internal_return());
        return;
    }
    store_flags(out);
    exit_registers();
    line(nx::format("if ({} == ret) return;", t));
    store_flags(static_cast<FlagSet>(jump & ~out));
    line(nx::format("psv_jump(ctx, {}, ret, {});", t, hex(in.address)));
    line("return;");
}

void Emitter::branch_to(const Instruction& in, u32 target_code, bool after) {
    (void)in;
    const RegSet exit = regs_ ? (after ? regs_->after_store[index_] : regs_->exit_store[index_]) : RegSet(0);
    const VfpSet vexit = vfp_ ? (after ? vfp_->after_store[index_] : vfp_->exit_store[index_]) : VfpSet{};
    if (auto stub = program_.import_stubs.find(target_code & ~1u); stub != program_.import_stubs.end()) {
        ++stats_.import_calls;
        store_flags(flags_.into_unknown(flags_fn_->live_out));
        store_registers(exit);
        store_vfp(vexit, stats_.vfp_stores);
        line(nx::format("psv_call_import(ctx, &{}_imports[{}], ret);", prefix_, stub->second));
        line("return;");
        return;
    }
    // (a branch to the function's own entry is a loop, in a replaced function's code, fo_<address>, too)
    if (target_code != fn_.entry && program_.function_at(target_code)) {
        const nx::string* host = replacement(target_code);
        ++(host ? stats_.replaced_tails : stats_.tail_calls);
        store_flags(flags_.into(target_code, flags_fn_->live_out));
        store_registers(exit);
        store_vfp(vexit, stats_.vfp_stores);
        line(nx::format("{}(ctx, ret);", host ? *host : function_name(target_code)));
        line("return;");
        return;
    }
    line(nx::format("goto {};", label_name(target_code)));
}

void Emitter::call(const Instruction& in, u32 target_code) {
    const u32 next = next_code(in);
    line(assignment(arm::kLr, hex(next)));
    call_registers_before();
    call_flags_before();
    if (auto stub = program_.import_stubs.find(target_code & ~1u); stub != program_.import_stubs.end()) {
        ++stats_.import_calls;
        line(nx::format("psv_call_import(ctx, &{}_imports[{}], {});", prefix_, stub->second, hex(next)));
    } else if (const nx::string* host = program_.function_at(target_code) ? replacement(target_code) : nullptr) {
        // A replaced function (Options::replacements): its host implementation (a guarded one's f_<address>)
        // directly. The register analysis took the call to have the AAPCS's contract (analysis::RegisterOptions::
        // hosts): every dirty register is in the context before it (EntryKind::Flushed), r4-r11 and sp are kept in
        // the locals, what else is live after it is read back. The flags and the floating-point registers are as at a
        // call of the function's code (which reads no flags at entry: generate). What the host function changes is
        // the AAPCS's: r0 (the result) and memory; a caller that reads r1-r3, r12, lr or a flag after the call before
        // writing it relies on what the guest's code leaves there (no compiler's call of a C function does): counted
        // and listed.
        ++stats_.replaced_calls;
        line(nx::format("{}(ctx, {});", *host, hex(next)));
        RegSet relied = 0;
        FlagSet relied_flags = 0;
        reads_after_call(fn_, cfg_, static_cast<u32>(index_), relied, relied_flags);
        if (relied || relied_flags) {
            ++stats_.replaced_relying;
            if (stats_.replacement_notes.size() < 40)
                stats_.replacement_notes.push_back(nx::format("0x{:08x}: the call of {} is followed by reads of {}{}{}", in.address, *host,
                                                              relied ? analysis::register_names(relied) : nx::string(), relied && relied_flags ? " and " : "",
                                                              relied_flags ? nx::format("flags {}", analysis::flag_names(relied_flags)) : nx::string()));
        }
    } else if (program_.function_at(target_code)) {
        const analysis::EntryKind ck = all_ && all_->internal_calls && regs_ && !regs_->call_entry.empty()
                                           ? static_cast<analysis::EntryKind>(regs_->call_entry[index_])
                                           : analysis::EntryKind::Public;
        const analysis::FunctionRegisters* callee = ck != analysis::EntryKind::Public ? all_->at(target_code) : nullptr;
        if (callee) {
            // (an internal entry: r0-r3, sp and lr it reads as arguments, r0/r1 it may change returned)
            const nx::string c = internal_call(target_code, *callee, "g", "");
            internal_callees_.insert(target_code);
            if (callee->results == 3)
                line(nx::format("{{ const uint64_t rv = {}; g0 = (uint32_t)rv; g1 = (uint32_t)(rv >> 32); }}", c));
            else if (callee->results == 1) line(nx::format("g0 = {};", c));
            else if (callee->results == 2) line(nx::format("g1 = {};", c));
            else line(c + ";");
            if (ck == analysis::EntryKind::Inside) ++stats_.calls_inside;
            else ++stats_.calls_flushed, ++stats_.calls_flushed_reasons[analysis::entry_reason_name(callee->reason)];
        } else {
            line(nx::format("{}(ctx, {});", function_name(target_code), hex(next)));
            if (all_ && all_->internal_calls) {
                const analysis::FunctionRegisters* c = all_->at(target_code);
                ++stats_.calls_public;
                ++stats_.calls_public_reasons[c ? analysis::entry_reason_name(c->reason) : "unknown"];
            }
        }
    } else {
        ++stats_.indirect_calls;
        line(indirect_call(hex(target_code), hex(next)));
    }
    call_registers_after(in);
    call_flags_after(in);
}

void Emitter::call_flags_before() {
    const FlagSet f = flags_fn_->before_call[index_];
    ++stats_.call_sites;
    if (f) ++stats_.call_sites_store;
    store_flags(f);
}

void Emitter::call_flags_after(const Instruction& in) {
    // (a call to a function that never returns: a trap if it does. The flags are the callee's then.)
    if (noreturn_call(in)) {
        line(nx::format("psv_trap(ctx, {}, PSV_TRAP_RETURNED);", hex(in.address + in.size)));
        ended_ = true;
        return;
    }
    const FlagSet f = flags_fn_->after_call[index_];
    if (f) ++stats_.call_sites_load;
    load_flags(f);
}

bool Emitter::constant_load(u32 address, u32 size, u32& value) const {
    return analysis::constant_word(program_, address, size, value);
}

bool Emitter::falls_through(const Instruction& in) const {
    const auto f = analysis::flow_of(in);
    if (f.conditional) return true;
    switch (f.kind) {
        case analysis::FlowKind::Next:
        case analysis::FlowKind::Call:
        case analysis::FlowKind::IndirectCall: return true;
        default: return false;
    }
}

namespace {

// The D registers an Advanced SIMD instruction may read or write (its helpers work on the context's), and the
// FPSCR's QC and AHP (the rest): more than it does for register lists (both spacings), never less.
VfpSet simd_context(const Instruction& in) {
    VfpSet s;
    s.fpscr = analysis::kFpscrRest;
    for (u8 i = 0; i < in.operand_count; ++i) {
        const Operand& o = in.operands[i];
        switch (o.kind) {
            case OperandKind::DReg:
            case OperandKind::DScalar: s.words |= analysis::d_words(o.reg & 31); break;
            case OperandKind::QReg: s.words |= analysis::d_words((2 * o.reg) & 31) | analysis::d_words((2 * o.reg + 1) & 31); break;
            case OperandKind::DRegList:
            case OperandKind::DLaneList: {  // (d<reg> on, by one or two, four at least)
                const u32 count = std::max<u32>(o.count, 4);
                for (u32 k = 0; k < 2 * count && o.reg + k < 32; ++k) s.words |= analysis::d_words(o.reg + k);
                break;
            }
            default: break;
        }
    }
    return s;
}

}  // namespace

void Emitter::instruction(const Instruction& in) {
    ended_ = false;
    access_ = {};
    vaccess_ = {};
    if (!in.valid()) return trap(in, "PSV_TRAP_UNDEFINED", "undefined");
    if (in.flags & arm::kUnpredictable) return trap(in, "PSV_TRAP_UNPREDICTABLE", nx::format("unpredictable {}", arm::mnemonic(in.op)));
    count_flags(in);
    if (emit_core(*this, in) || emit_memory(*this, in) || emit_vfp(*this, in)) return;
    if (neon_locals()) {
        const usize before = body_.size();
        if (emit_neon(*this, in)) {
            ++stats_.neon_values;
            return;
        }
        // (no translation on values: the context form below, the locals synced around it)
        body_.resize(before);
        vaccess_ = {};
        access_ = {};
        temp_counter_ = 0;
    }
    const usize mark = body_.size();
    if (emit_simd(*this, in)) {
        // (Advanced SIMD works on the context's registers: the locals it reads in the context first, those it may
        // have written read back after)
        if (vfp_mode_ == VfpMode::Probe) vaccess_.context = simd_context(in);
        if (vfp_) {
            const VfpSet before = vfp_->call_store[index_], after = vfp_->call_load[index_];
            if (!before.empty()) body_.insert(mark, "        " + vfp_line(before, true) + "\n");
            if (!after.empty()) line(vfp_line(after, false));
            stats_.vfp_stores += !before.empty();
            stats_.vfp_call_loads += !after.empty();
            stats_.vfp_simd_syncs += !before.empty() || !after.empty();
        }
        if (vfp_mode_ != VfpMode::Context) ++stats_.neon_context;
        flushed_ = 0;  // (the locals read back from the context)
        return;
    }
    unimplemented(in);
}

void Emitter::count_flags(const Instruction& in) {
    const FlagSet defs = analysis::flag_effect(in).defs;
    if (!defs) return;
    ++stats_.flag_setters;
    if (defs & live()) ++stats_.flag_setters_live;
}

nx::string Emitter::function() {
    const auto& code = fn_.code;
    if (code.empty()) {  // (a function with no code: synthetic programs' placeholders)
        ++stats_.functions;
        return nx::format("\nvoid {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret) {{ (void)ctx; (void)ret; }}\n", name_.empty() ? function_name(fn_.entry) : name_);
    }
    // Labels: branch targets, and the places control continues at that are not right after the instruction before.
    for (u32 l : fn_.labels) labels_.insert(l);
    nx::flat_hash_set<u32> addresses;
    for (const auto& in : code) addresses.insert(in.address);
    // (a call to a function that never returns ends the flow when it executes: see call_flags_after)
    nx::flat_hash_set<u32> noreturn(fn_.noreturn_calls.begin(), fn_.noreturn_calls.end());
    for (usize i = 0; i < code.size(); ++i) {
        const auto& in = code[i];
        const u32 next = in.address + in.size;
        if (falls_through(in) && !fn_.runs_into.contains(in.address) && !(noreturn.contains(in.address) && !analysis::is_conditional(in)) &&
            (i + 1 >= code.size() || code[i + 1].address != next) && addresses.contains(next))
            labels_.insert(next);
    }
    const bool entry_first = !code.empty() && code.front().address == fn_.start();
    if (!entry_first) labels_.insert(fn_.start());

    nx::string out;
    if (!entry_first) out += nx::format("    goto {};\n", label_name(fn_.start()));
    if (mode_ == RegisterMode::Probe) effects_.assign(code.size(), {});
    if (vfp_mode_ == VfpMode::Probe) vfp_effects_.assign(code.size(), {});
    if (vfp_locals()) plan_fused_compares();
    for (usize i = 0; i < code.size(); ++i) {
        const auto& in = code[i];
        index_ = i;
        ++stats_.instructions;
        if (labels_.contains(in.address)) out += nx::format("{}:;\n", label_name(in.address));
        body_.clear();
        temp_counter_ = 0;
        if (labels_.contains(in.address)) flushed_ = 0;
        flushed_out_ = 0;
        instruction(in);
        {
            // (what the instruction leaves known of the D registers' lanes: see known_flushed)
            u32 defs = 0;
            for (u32 n = 0; n < 32; ++n)
                if (vaccess_.defs.words & analysis::d_words(n)) defs |= 1u << n;
            if (cfg_.nodes[i].call != analysis::CallKind::None || !vaccess_.context.empty() || in.op == Op::It) flushed_ = 0;
            else if (analysis::is_conditional(in)) flushed_ &= ~defs;
            else flushed_ = (flushed_ & ~defs) | (flushed_out_ & defs);
        }
        if (mode_ == RegisterMode::Probe) effects_[i] = access_;
        if (vfp_mode_ == VfpMode::Probe) vfp_effects_[i] = vaccess_;
        // (an IT instruction's condition is its block's first condition, not its own; after a fused compare, the
        // operands compared)
        nx::string cond = in.op == Op::It ? nx::string() : condition(in.cond);
        if (!cond.empty() && !fused_cond_.empty() && fused_cond_[i]) {
            cond = fused_condition(in.cond, fused_cond_[i]);
            use_fused(fused_cond_[i]);
            ++stats_.vfp_fused_conditions;
        }
        const nx::string text = comment_text(arm::format(in));
        if (cond.empty()) out += nx::format("    {{ /* {:08x}: {} */\n", in.address, text);
        else out += nx::format("    if ({}) {{ /* {:08x}: {} */\n", cond, in.address, text);
        out += body_;
        out += "    }\n";
        // (a translation that ends in a trap ends the flow only where the instruction is not conditional)
        if (!falls_through(in) || (ended_ && !analysis::is_conditional(in))) continue;
        const u32 next = in.address + in.size;
        if (auto into = fn_.runs_into.find(in.address); into != fn_.runs_into.end()) {
            body_.clear();
            branch_to(in, into->second, true);  // (a code address, or an import stub's address)
            out += "    /* runs into the next function */\n" + body_;
            continue;
        }
        if (i + 1 < code.size() && code[i + 1].address == next) continue;
        if (addresses.contains(next)) {
            out += nx::format("    goto {};\n", label_name(next));
        } else {
            nx::string stores = flags_line(analysis::kAllFlags, true);
            if (regs_ && regs_->after_store[i]) {
                stores += " " + register_line(regs_->after_store[i], true);
                stats_.register_trap_stores += static_cast<usize>(std::popcount(regs_->after_store[i]));
            }
            if (vfp_ && !vfp_->after_store[i].empty()) {
                stores += " " + vfp_line(vfp_->after_store[i], true);
                ++stats_.vfp_trap_stores;
            }
            out += "    " + stores + "\n";
            out += nx::format("    psv_trap(ctx, {}, PSV_TRAP_UNDISCOVERED);\n", hex(next));
        }
    }
    ++stats_.functions;
    if (flags_fn_->need) ++stats_.functions_need;
    if (flags_fn_->live_out) ++stats_.functions_live_out;
    // The guest memory base, read once (see guest.h: psv_membase never changes while guest code runs, but every guest
    // store could change it as far as the compiler knows). The condition flags in a local (analysis/flags.hpp): those
    // live at entry from the context, the others 0 (no instruction reads them before writing them; a trap shows 0).
    const bool memory = out.find("(mb,") != nx::string::npos;
    nx::string flags;
    for (const auto& [bit, name] : kFlagNames)
        flags += nx::format("{}{}", flags.empty() ? "" : ", ", (flags_fn_->live_in & bit) ? nx::format("ctx->{}", name) : nx::string("0u"));
    // The registers in locals (analysis/registers.hpp): those read before they are written from the context, the
    // others 0 (never read before they are written).
    nx::string registers;
    if (regs_) {
        const RegSet params = internal_body() ? regs_->params : RegSet(0);
        for (u32 k = 0; k < 15; ++k) {
            if (!(regs_->touched & (1u << k)) || (params & (1u << k))) continue;
            const bool load = regs_->load_at_entry & (1u << k);
            registers += nx::format("{}g{} = {}", registers.empty() ? "    uint32_t " : ", ", k, load ? nx::format("ctx->r[{}]", k) : nx::string("0"));
        }
        if (!registers.empty()) registers += ";\n";
        stats_.register_entry_loads += static_cast<usize>(std::popcount(regs_->load_at_entry));
    }
    // The floating-point registers and the FPSCR in locals (analysis/vfp.hpp): those live at entry from the context,
    // the others 0 (never read before written); the operands of fused compares.
    nx::string vfp;
    if (vfp_) {
        nx::string floats, doubles, vectors;
        const VfpSet touched = vfp_->touched, load = vfp_->load_at_entry;
        for (u32 n = 0; n < 32; ++n) {
            if (vfp_->q_held_d(n)) {
                if ((n & 1) || !(touched.words & analysis::q_words(n / 2))) continue;
                const bool in = (load.words & analysis::q_words(n / 2)) != 0;
                vectors += nx::format("{}q{} = {}", vectors.empty() ? "    PsvQ " : ", ", n / 2,
                                      in ? nx::format("psv_ctx_qget(ctx, {})", n) : nx::string("(PsvQ){0, 0, 0, 0}"));
                stats_.vfp_entry_loads += in;
                ++stats_.vfp_q_held;
            } else if (vfp_->s_held(n)) {
                bool any = false;
                for (u32 k = 2 * n; k <= 2 * n + 1; ++k) {
                    if (!(touched.words & (1ull << k))) continue;
                    const bool in = (load.words >> k) & 1;
                    floats += nx::format("{}s{} = {}", floats.empty() ? "    float " : ", ", k, in ? nx::format("psv_f32(ctx->vfp.s[{}])", k) : nx::string("0"));
                    stats_.vfp_entry_loads += in;
                    any = true;
                }
                stats_.vfp_s_held += any;
            } else if (touched.words & analysis::d_words(n)) {
                const bool in = (load.words & analysis::d_words(n)) != 0;
                doubles += nx::format("{}d{} = {}", doubles.empty() ? "    double " : ", ", n, in ? nx::format("psv_f64(ctx->vfp.d[{}])", n) : nx::string("0"));
                stats_.vfp_entry_loads += in;
                ++stats_.vfp_d_held;
            }
        }
        if (!floats.empty()) vfp += floats + ";\n";
        if (!doubles.empty()) vfp += doubles + ";\n";
        if (!vectors.empty()) vfp += vectors + ";\n";
        if (touched.fpscr & analysis::kFpscrRest)
            vfp += nx::format("    uint32_t fpscr = {};\n", (load.fpscr & analysis::kFpscrRest) ? "ctx->fpscr" : "0");
        if (touched.fpscr & analysis::kFpscrNzcv)
            vfp += nx::format("    uint32_t fpnzcv = {};\n", (load.fpscr & analysis::kFpscrNzcv) ? "ctx->fpscr & 0xF0000000u" : "0");
        stats_.vfp_entry_loads += static_cast<usize>(std::popcount(static_cast<u32>(load.fpscr)));
        if (uses_fc32_) vfp += "    float fc32a = 0, fc32b = 0;\n";
        if (uses_fc64_) vfp += "    double fc64a = 0, fc64b = 0;\n";
        if (!touched.empty()) ++stats_.vfp_functions;
        stats_.vfp_calls_keeping += vfp_->calls_keeping;
    }
    if (sp_trap_) out += nx::format("L_SP_MOVED:;\n    psv_trap(ctx, {}, PSV_TRAP_STACK_POINTER);\n", hex(fn_.start()));
    const nx::string locals = nx::format("{}    PsvFlags fl = {{{}}};\n{}{}", memory ? "    uint8_t* const mb = psv_membase;\n" : "", flags, registers, vfp);
    if (!internal_body())
        return nx::format("\nvoid {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret) {{\n", name_.empty() ? function_name(fn_.entry) : name_) + locals + out + "}\n";
    // An internal entry (analysis::EntryKind), then f_<address> around it: from the context's registers, its results
    // back into it, and a jump through the runtime to the LR it was entered with when that is not ret (what the
    // function's own returns did before). Every way through the code ends in a return or a trap; one more trap at the end
    // (never reached) keeps C from running off a function that returns a value.
    ++(regs_->entry == analysis::EntryKind::Inside ? stats_.internal_inside : stats_.internal_flushed);
    stats_.internal_params += static_cast<usize>(std::popcount(regs_->params));
    stats_.internal_results += static_cast<usize>(std::popcount(regs_->results));
    // (a small one inlined: a static copy in each file that calls it, the wrapper apart; generate)
    nx::string text = nx::format("\n{}{} {{\n", inline_ ? "static inline " : "", internal_signature(fn_.entry, *regs_)) + locals + out;
    text += nx::format("    psv_trap(ctx, {}, PSV_TRAP_UNDISCOVERED);\n}}\n", hex(fn_.start()));
    nx::string& w = inline_ ? wrapper_ : text;
    // (the definition's text differs from that of a function without an internal entry, so that a game's script that
    // renames that definition to stand in for the function finds nothing: direct calls reach the internal entry, so such
    // a function must be given none, psvrecomp --no-inline)
    w += nx::format("\nvoid {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret)\n{{\n    const uint32_t lr = ctx->r[14];\n", name_.empty() ? function_name(fn_.entry) : name_);
    const nx::string c = internal_call(fn_.entry, *regs_, "ctx->r[", "]");
    if (regs_->results == 3) w += nx::format("    const uint64_t rv = {};\n    ctx->r[0] = (uint32_t)rv; ctx->r[1] = (uint32_t)(rv >> 32);\n", c);
    else if (regs_->results == 1) w += nx::format("    ctx->r[0] = {};\n", c);
    else if (regs_->results == 2) w += nx::format("    ctx->r[1] = {};\n", c);
    else w += nx::format("    {};\n", c);
    w += nx::format("    if (__builtin_expect(lr != ret, 0)) psv_jump(ctx, lr, ret, {});\n}}\n", hex(fn_.start()));
    return text;
}

}  // namespace detail

namespace {

nx::string module_source(const analysis::Program& program, const Options& options, const analysis::FlagLiveness& flags) {
    const nx::string& p = options.name;
    nx::string out = nx::format("/* Generated by psvrecomp: module {}. */\n#include \"{}_functions.h\"\n\n", program.image->module().info.name, p);
    out += nx::format("PsvImport {}_imports[{}] = {{\n", p, std::max<usize>(program.imports.size(), 1));
    for (const auto& imp : program.imports)
        out += nx::format("    {{0x{:08X}u, 0x{:08X}u, \"{}\", 0}},\n", imp.library_nid, imp.nid, imp.library);
    if (program.imports.empty()) out += "    {0, 0, 0, 0},\n";
    out += "};\n\n";
    nx::vector<u32> stubs(program.imports.size(), 0);
    for (const auto& [address, index] : program.import_stubs) stubs[index] = address;
    out += nx::format("static const uint32_t {}_import_stubs[{}] = {{\n", p, std::max<usize>(stubs.size(), 1));
    for (u32 s : stubs) out += nx::format("    {},\n", hex(s));
    if (stubs.empty()) out += "    0,\n";
    out += "};\n\n";
    out += nx::format("static const PsvFunctionEntry {}_function_table[{}] = {{\n", p, std::max<usize>(program.functions.size(), 1));
    for (const auto& [code, fn] : program.functions) out += nx::format("    {{{}, {}}},\n", hex(code), function_name(code));
    if (program.functions.empty()) out += "    {0, 0},\n";
    out += "};\n\n";
    out += nx::format("PsvVariable {}_variables[{}] = {{\n", p, std::max<usize>(program.variable_imports.size(), 1));
    for (const auto& v : program.variable_imports)
        out += nx::format("    {{0x{:08X}u, 0x{:08X}u, \"{}\", 0}},\n", v.library_nid, v.nid, v.library);
    if (program.variable_imports.empty()) out += "    {0, 0, 0, 0},\n";
    out += "};\n\n";
    const auto& segments = program.image->segments();
    out += nx::format("static const uint32_t {}_segment_bases[{}] = {{", p, std::max<usize>(segments.size(), 1));
    for (const auto& s : segments) out += nx::format(" {},", hex(s.base));
    if (segments.empty()) out += " 0";
    out += " };\n";
    out += nx::format("static const uint32_t {}_segment_sizes[{}] = {{", p, std::max<usize>(segments.size(), 1));
    for (const auto& s : segments) out += nx::format(" {},", hex(s.size()));
    if (segments.empty()) out += " 0";
    out += " };\n\n";
    nx::string fingerprint;
    for (u8 b : program.image->fingerprint()) fingerprint += nx::format("0x{:02X},", b);
    // (the condition flags' contract at the module's boundaries: guest.h, PsvModuleCode; the indirect-call table, which
    // registering the module fills: PsvDispatch)
    if (options.call_table)
        out += nx::format("PsvDispatch {0}_dispatch;\n\n/* (indirect calls: guest.h, psv_call_in) */\nvoid {0}_call(PsvContext* PSV_RESTRICT ctx, uint32_t target, "
                          "uint32_t ret) {{ psv_call_in(&{0}_dispatch, ctx, target, ret); }}\n\n", p);
    out += nx::format("const PsvModuleCode {}_module = {{\"{}\", {}_function_table, {}u, {}_imports, {}u, {}_import_stubs, {}_variables, {}u,\n"
                      "    {{{}}}, {}_segment_bases, {}_segment_sizes, {}u,\n    PSV_FLAG_CONTRACT, 0x{:X}u, 0x{:X}u, 0x{:X}u, 0x{:X}u{}}};\n",
                      p, program.image->module().info.name, p, program.functions.size(), p, program.imports.size(), p, p,
                      program.variable_imports.size(), fingerprint, p, p, segments.size(), flags.needed_at_entry, flags.given_at_entry,
                      flags.needed_at_return, flags.given_at_return, options.call_table ? nx::format(", &{}_dispatch", p) : nx::string());
    return out;
}

}  // namespace

Output generate(const analysis::Program& original, const Options& options) {
    Output result;
    const nx::string& p = options.name;
    Stats& st = result.stats;
    const analysis::Program& program = original;
    analysis::FlagOptions flag_options;
    flag_options.everywhere = options.flags_everywhere;
    flag_options.given_at_entry = options.flags_given_at_entry;
    flag_options.given_at_return = options.flags_given_at_return;
    flag_options.full_exit = options.full_exit_flags;
    analysis::FlagLiveness flags = analysis::flag_liveness(program, flag_options);
    // Host implementations (Options::replacements): a replaced function keeps its place among the functions (f_<address>
    // stays in the function table), and its address its calls and branches; the register analysis takes a call or tail
    // call of it to have the AAPCS's contract (analysis::RegisterOptions::hosts) and the Emitter calls the host function
    // there. It gets no internal entry (so it is never inlined): a guarded one's code, fo_<address>, is a function
    // entered with every register in the context. A function whose code reads flags at entry (analysis::FunctionFlags::
    // need) is not a C function: not replaced.
    nx::flat_hash_map<u32, nx::string> host_callees;  // code address (bit 0 clear) -> the C function called
    nx::map<u32, const Options::Replacement*> replaced;  // by code address
    nx::map<u32, bool> hosts;                            // (analysis::RegisterOptions::hosts)
    nx::vector<u32> no_internal = options.no_internal;
    for (const auto& r : options.replacements) {
        const analysis::Function* fn = original.function_at(r.code);
        if (!fn || fn->entry != r.code) {
            st.replacement_notes.push_back(nx::format("0x{:08x} ({}): no function starts there; not replaced", r.code, r.symbol));
            continue;
        }
        if (original.import_stubs.contains(r.code & ~1u) || replaced.contains(r.code)) {
            st.replacement_notes.push_back(nx::format("0x{:08x} ({}): already replaced, or an import's stub; not replaced", r.code, r.symbol));
            continue;
        }
        if (const auto* ff = flags.at(r.code); ff && ff->need) {
            st.replacement_notes.push_back(nx::format("0x{:08x} ({}): its code reads flags {} at entry; not replaced", r.code, r.symbol,
                                                      analysis::flag_names(ff->need)));
            continue;
        }
        replaced.emplace(r.code, &r);
        hosts[r.code] = !r.guard.empty();
        host_callees[r.code & ~1u] = r.guard.empty() ? r.symbol : function_name(r.code);
        no_internal.push_back(r.code & ~1u);
        ++st.replaced;
        st.replaced_guarded += !r.guard.empty();
    }
    nx::string header = nx::format(
        "/* Generated by psvrecomp: module {}. */\n#pragma once\n#include \"psvrecore/recomp/guest.h\"\n\n"
        "extern PsvImport {}_imports[];\nextern PsvVariable {}_variables[];\nextern const PsvModuleCode {}_module;\n{}\n",
        program.image->module().info.name, p, p, p, options.call_table ? nx::format("extern PsvDispatch {0}_dispatch;\nvoid {0}_call(PsvContext* PSV_RESTRICT ctx, uint32_t target, uint32_t ret);\n", p) : nx::string());
    for (const auto& [code, fn] : program.functions) header += nx::format("void {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret);\n", function_name(code));
    // (the host functions and guards; guest.h declares the runtime's, psv_host_*, with their DLL linkage)
    {
        nx::flat_hash_set<nx::string> declared;
        for (const auto& [code, r] : replaced) {
            if (!r->guard.empty()) header += nx::format("void {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret);\n", guest_code_name(code));
            if (!r->symbol.starts_with("psv_host_") && declared.insert(r->symbol).second)
                header += nx::format("void {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret);\n", r->symbol);
            if (!r->guard.empty() && !r->guard.starts_with("psv_host_") && declared.insert(r->guard).second)
                header += nx::format("int {}(const PsvContext* ctx);\n", r->guard);
        }
    }
    result.files.push_back(GeneratedFile{p + "_functions.h", nx::string()});

    // The .c files: each function's definition (a small internal entry's wrapper alone), and the functions inlined
    // whose internal entries the file's functions call, a static copy each (with those they call), put in front at the
    // end, when every function's text is known.
    struct Chunk {
        nx::string text;
        nx::flat_hash_set<u32> uses;  // inlined functions it calls
    };
    nx::vector<Chunk> chunks;
    Chunk chunk;
    u32 chunk_instructions = 0;
    const auto flush = [&] {
        if (chunk.text.empty()) return;
        chunks.push_back(std::move(chunk));
        chunk = Chunk{};
        chunk_instructions = 0;
    };
    // The registers (analysis/registers.hpp): a first translation records what each instruction's translation reads
    // and writes (checked against analysis::register_effect), the analysis runs on that, then the translation.
    // The floating-point registers likewise (analysis/vfp.hpp): what the first translation reads and writes of them,
    // then their analysis (on the registers', which says which functions return where they were called from).
    const bool locals = !options.registers_in_context;
    const bool vfp_locals = !options.vfp_in_context;
    nx::map<u32, nx::flat_hash_set<u32>> inline_callees;  // (an inlined function: the internal entries it calls)
    nx::map<u32, nx::string> inline_bodies;
    analysis::RegisterAnalysis registers;
    analysis::VfpAnalysis vfp;
    nx::map<u32, nx::vector<analysis::VfpEffect>> vfp_effects;
    if (vfp_locals && !locals) {
        Stats scratch;
        for (const auto& [code, fn] : program.functions) {
            Emitter e(program, fn, flags, p, scratch, RegisterMode::Probe, nullptr, VfpMode::Probe);
            e.set_neon_locals(!options.neon_in_context);
            e.set_replacements(&host_callees);
            (void)e.function();
            vfp_effects[code] = e.vfp_effects();
        }
    }
    if (locals) {
        nx::map<u32, nx::vector<analysis::RegisterEffect>> effects;
        Stats scratch;
        for (const auto& [code, fn] : program.functions) {
            Emitter e(program, fn, flags, p, scratch, RegisterMode::Probe, nullptr, vfp_locals ? VfpMode::Probe : VfpMode::Context);
            e.set_neon_locals(!options.neon_in_context);
            e.set_replacements(&host_callees);
            (void)e.function();
            if (vfp_locals) vfp_effects[code] = e.vfp_effects();
            const auto& got = e.effects();
            for (usize i = 0; i < got.size(); ++i) {
                const analysis::RegisterEffect model = analysis::register_effect(fn.code[i]);
                // (a table branch traps for an index outside its table: the model does not know the tables)
                const bool traps = model.traps || fn.tables.contains(fn.code[i].address);
                const bool fits = got[i].defs == model.defs && (got[i].uses & ~model.uses) == 0 && (!got[i].traps || traps);
                if (fits) continue;
                ++st.register_effect_mismatches;
                if (st.register_notes.size() < 20)
                    st.register_notes.push_back(nx::format("0x{:08x} {}: translation reads {} writes {}{}, analysis::register_effect reads {} writes {}{}",
                                                           fn.code[i].address, arm::format(fn.code[i]), analysis::register_names(got[i].uses),
                                                           analysis::register_names(got[i].defs), got[i].traps ? " (traps)" : "",
                                                           analysis::register_names(model.uses), analysis::register_names(model.defs),
                                                           model.traps ? " (traps)" : ""));
            }
            effects[code] = got;
        }
        analysis::RegisterOptions register_options;
        register_options.callee_saved = !options.no_callee_saved;
        register_options.keep = options.keep_registers;
        register_options.check_sp = options.check_sp;
        register_options.effects = &effects;
        register_options.internal_calls = options.internal_calls;
        register_options.no_internal = &no_internal;
        register_options.hosts = &hosts;
        registers = analysis::register_analysis(program, register_options);
        for (const auto& [code, r] : registers.functions) {
            const auto* fn = program.function_at(code);
            if (!fn || fn->code.empty()) continue;
            for (u32 k = 4; k < 14; ++k)
                if (r.preserved & (1u << k)) ++st.functions_preserving[k];
            if ((r.preserved & analysis::kPreservable) == analysis::kPreservable) ++st.functions_preserving_all;
            if (r.checks_sp) ++st.functions_checking_sp;
            if (r.entry == analysis::EntryKind::Public) ++st.public_reasons[analysis::entry_reason_name(r.reason)];
            else if (r.entry == analysis::EntryKind::Flushed) ++st.flushed_reasons[analysis::entry_reason_name(r.reason)];
        }
    }
    // Small internal entries are inlined: no jump table, at most inline_limit instructions (analysis::EntryKind; not the
    // functions given no internal entry, psvrecomp --no-inline).
    nx::flat_hash_set<u32> inlined;
    if (locals && registers.internal_calls)
        for (const auto& [code, fn] : program.functions) {
            const auto* r = registers.at(code);
            if (r && r->entry != analysis::EntryKind::Public && fn.tables.empty() && fn.code.size() <= options.inline_limit) inlined.insert(code);
        }
    // (the other internal entries' prototypes; the inlined ones are static in each file. The others are not inlined by
    // the C compiler into the callers and the wrapper of their own file: a copy there keeps the out-of-line one too,
    // which the other files call)
    if (registers.internal_calls)
        for (const auto& [code, r] : registers.functions)
            if (r.entry != analysis::EntryKind::Public && !inlined.contains(code))
                header += nx::format("{}{};\n", options.noinline_internal ? "__attribute__((noinline)) " : "", internal_signature(code, r));
    result.files.front().text = std::move(header);
    if (vfp_locals) {
        analysis::VfpOptions vfp_options;
        vfp_options.summaries = !options.vfp_calls_conservative;
        vfp_options.q_view = !options.no_q_locals;
        vfp_options.effects = &vfp_effects;
        vfp_options.registers = locals ? &registers : nullptr;
        vfp = analysis::vfp_analysis(program, vfp_options);
    }
    for (const auto& [code, fn] : program.functions) {
        Emitter e(program, fn, flags, p, st, locals ? RegisterMode::Locals : RegisterMode::Context, registers.at(code),
                  vfp_locals ? VfpMode::Locals : VfpMode::Context, vfp.at(code));
        e.set_neon_locals(!options.neon_in_context);
        e.set_call_table(options.call_table, options.call_table_inline);
        e.set_replacements(&host_callees);
        if (locals) e.set_register_analysis(&registers);
        const bool small = inlined.contains(code);
        e.set_inline(small);
        if (const auto rep = replaced.find(code); rep != replaced.end()) {
            // A replaced function's f_<address>: the host function; with a guard, the guest code's translation
            // (fo_<address>, a function without an internal entry: Options::replacements) where the guard does not hold.
            // fo_<address> reads at entry what any function entered from outside may (the contract's flags given at
            // entry, every register from the context) and leaves what it changes in the context, as every f_<address>
            // does; its callers here are only this wrapper.
            const Options::Replacement& r = *rep->second;
            nx::string text;
            if (r.guard.empty()) {
                text = nx::format("\nvoid {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret) {{ {}(ctx, ret); }}  /* host implementation (psvrecomp --replace) */\n",
                                  function_name(code), r.symbol);
            } else {
                e.set_name(guest_code_name(code));
                text = e.function();
                for (u32 c : e.internal_callees())  // (fo_<address>'s calls of inlined functions: their copies in this file)
                    if (inlined.contains(c)) chunk.uses.insert(c), ++st.calls_inlined;
                text += nx::format("\nvoid {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret) {{\n"
                                   "    if ({}(ctx)) {{ {}(ctx, ret); return; }}  /* host implementation (psvrecomp --replace) */\n"
                                   "    {}(ctx, ret);\n}}\n",
                                   function_name(code), r.guard, r.symbol, guest_code_name(code));
            }
            chunk.text += text;
            chunk_instructions += static_cast<u32>(fn.code.size());
            if (chunk_instructions >= options.instructions_per_file) flush();
            continue;
        }
        nx::string text = e.function();
        for (u32 c : e.internal_callees())
            if (inlined.contains(c)) chunk.uses.insert(c), ++st.calls_inlined;
        if (small) {
            inline_callees[code] = e.internal_callees();
            inline_bodies[code] = std::move(text);
            chunk.text += e.wrapper();
            chunk.uses.insert(code);
        } else {
            chunk.text += text;
        }
        chunk_instructions += static_cast<u32>(fn.code.size());
        if (chunk_instructions >= options.instructions_per_file) flush();
    }
    flush();
    st.functions_inlined = inlined.size();
    for (usize k = 0; k < chunks.size(); ++k) {
        // (the inlined functions the file calls, and those they call)
        nx::vector<u32> work(chunks[k].uses.begin(), chunks[k].uses.end());
        nx::flat_hash_set<u32> all(chunks[k].uses.begin(), chunks[k].uses.end());
        while (!work.empty()) {
            const u32 c = work.back();
            work.pop_back();
            for (u32 d : inline_callees[c])
                if (inlined.contains(d) && all.insert(d).second) work.push_back(d);
        }
        nx::vector<u32> sorted(all.begin(), all.end());
        std::sort(sorted.begin(), sorted.end());
        nx::string front;
        for (u32 c : sorted) front += "static inline " + internal_signature(c, *registers.at(c)) + ";\n";
        for (u32 c : sorted) front += inline_bodies[c];
        st.inline_copies += sorted.size();
        result.files.push_back(GeneratedFile{nx::format("{}_code_{}.c", p, k), nx::format("/* Generated by psvrecomp. */\n#include \"{}_functions.h\"\n", p) +
                                                                                 front + chunks[k].text});
    }
    result.files.push_back(GeneratedFile{p + "_module.c", module_source(original, options, flags)});
    // (the summaries only: the per-instruction sets are the generator's business)
    for (auto& [code, f] : flags.functions) {
        nx::vector<FlagSet>().swap(f.after);
        nx::vector<FlagSet>().swap(f.after_call);
        nx::vector<FlagSet>().swap(f.before_call);
        nx::vector<FlagSet>().swap(f.live_before);
    }
    result.flags = std::move(flags);
    for (auto& [code, r] : registers.functions) {
        for (auto* v : {&r.trap_store, &r.call_store, &r.call_load, &r.call_kept, &r.exit_store, &r.after_store, &r.live_before})
            nx::vector<RegSet>().swap(*v);
        nx::vector<u8>().swap(r.call_entry);
    }
    result.registers = std::move(registers);
    return result;
}

}  // namespace psvrecore::recomp
