#include "psvrecore/analysis/program.hpp"

#include <algorithm>

#include "psvrecore/analysis/flow.hpp"
#include "psvrecore/arm/decode.hpp"
#include "psvrecore/arm/format.hpp"
#include "core/foundation/containers/hash_set.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::analysis {

using arm::Instruction;
using arm::Mode;
using arm::Op;
using arm::OperandKind;

bool is_noreturn_name(nx::string_view name) {
    static constexpr const char* kNames[] = {
        "abort", "exit", "_Exit", "quick_exit", "longjmp", "_longjmp", "siglongjmp", "__stack_chk_fail",
        "__cxa_throw", "__cxa_rethrow", "__cxa_bad_cast", "__cxa_bad_typeid", "__cxa_pure_virtual",
        "__cxa_deleted_virtual", "__cxa_call_unexpected", "_Unwind_Resume", "_ZSt9terminatev", "_ZSt10unexpectedv",
        // The C++ library's helpers that throw its standard exceptions.
        "_ZSt11_Xbad_allocv", "_ZSt12_Xbad_typeidv", "_ZSt10_Xbad_castv", "_ZSt14_Xlength_errorPKc",
        "_ZSt14_Xout_of_rangePKc", "_ZSt16_Xinvalid_argumentPKc", "_ZSt13_Xrange_errorPKc",
        "_ZSt14_Xoverflow_errorPKc", "_ZSt15_Xruntime_errorPKc",
        "sceKernelExitProcess", "sceKernelExitThread", "sceKernelExitDeleteThread",
    };
    for (const char* n : kNames)
        if (name == n) return true;
    return false;
}

nx::vector<u32> exidx_entries(const Image& image) {
    nx::vector<u32> out;
    const auto& info = image.module().info;
    if (!info.exidx_start || info.exidx_end <= info.exidx_start) return out;
    for (u32 at = info.exidx_start; at + 8 <= info.exidx_end; at += 8) {
        const u32 word = image.u32_at(at);
        const u32 offset = (word & 0x7FFFFFFFu) | ((word & 0x40000000u) << 1);  // prel31, sign-extended
        out.push_back(at + offset);
    }
    return out;
}

namespace {

class Analyzer {
public:
    Analyzer(const Image& image, const Options& options, Program& program)
        : image_(image), options_(options), p_(program) {}

    void run();

private:
    struct Traversal {
        Function fn;
        nx::map<u32, Instruction> code;  // by address
        nx::flat_hash_set<u32> labels;
        nx::vector<Instruction> deferred;  // table branches without a bounds check, resolved last
        bool may_return = false;             // reaches an indirect branch other than a table branch
        nx::vector<u32> tail_targets;        // functions (or import stubs) it may end in
    };

    bool valid_entry(u32 code, nx::string* why) const;
    bool is_entry(u32 code) const { return entries_.contains(code); }
    bool is_noreturn(u32 code) const {
        if (auto stub = p_.import_stubs.find(code & ~1u); stub != p_.import_stubs.end()) return noreturn_imports_.contains(stub->second);
        return noreturn_.contains(code);
    }
    // Which functions return, from what each one's code ends in; true when the set changed.
    bool update_noreturn();
    void add_entry(u32 code, u32 from, const char* source);
    Function traverse(u32 entry);
    Instruction decode_at(u32 address, Mode mode, arm::ItState& it, bool& ok) const;
    // `unbounded`: a table branch without a bounds check is read up to the code after its table (once the rest of
    // the function is known); without it, such a table is not read.
    bool jump_table(Traversal& t, const Instruction& in, JumpTable& out, bool unbounded);
    u32 bound_for(const Traversal& t, const Instruction& site, u8 index_reg, bool& found) const;
    void diag(Severity s, u32 address, u32 function, nx::string message) {
        p_.diagnostics.push_back(Diagnostic{s, address, function, std::move(message)});
    }

    const Image& image_;
    const Options& options_;
    Program& p_;
    nx::flat_hash_set<u32> entries_;   // code addresses
    nx::vector<u32> pending_;          // entries found during a pass
    nx::flat_hash_set<u32> rejected_;  // (each invalid target reported once)
    nx::flat_hash_set<u32> noreturn_;          // functions (code addresses) that never return
    nx::flat_hash_set<u32> noreturn_imports_;  // indices of imported functions that never return
    struct Exits {
        bool may_return = false;
        nx::vector<u32> tail_targets;
    };
    nx::flat_hash_map<u32, Exits> exits_;  // of each function, from its latest traversal
    bool collect_diagnostics_ = false;  // only in the final pass
};

bool Analyzer::valid_entry(u32 code, nx::string* why) const {
    const u32 a = code & ~1u;
    const Mode mode = code_mode(code);
    if (mode == Mode::Arm && (code & 3) != 0) {
        if (why) *why = "an ARM address that is not word-aligned";
        return false;
    }
    if (!image_.executable(a)) {
        if (why) *why = "not in an executable segment";
        return false;
    }
    return true;
}

void Analyzer::add_entry(u32 code, u32 from, const char* source) {
    if (p_.import_stubs.contains(code & ~1u)) return;
    if (entries_.contains(code)) return;
    nx::string why;
    if (!valid_entry(code, &why)) {
        if (rejected_.insert(code).second)
            diag(Severity::Error, code, from, nx::format("{} 0x{:08x} is {}", source, code, why));
        return;
    }
    entries_.insert(code);
    pending_.push_back(code);
}

Instruction Analyzer::decode_at(u32 address, Mode mode, arm::ItState& it, bool& ok) const {
    ok = false;
    if (mode == Mode::Arm) {
        if (!image_.contains(address, 4) || !image_.executable(address)) return {};
        ok = true;
        return arm::decode_arm(image_.u32_at(address), address);
    }
    if (!image_.contains(address, 2) || !image_.executable(address)) return {};
    const u16 hw1 = image_.u16_at(address);
    u16 hw2 = 0;
    if (arm::thumb_is_32bit(hw1)) {
        if (!image_.contains(address + 2, 2)) return {};
        hw2 = image_.u16_at(address + 2);
    }
    ok = true;
    return arm::decode_thumb(hw1, hw2, address, it);
}

// The number of entries a table branch indexed by `index_reg` has, from the bounds check before it:
// cmp rX, #N followed by a branch away when rX > N (bhi) or rX >= N (bhs), or a table branch executed only when
// rX <= N (ldrls pc, ...).
u32 Analyzer::bound_for(const Traversal& t, const Instruction& site, u8 index_reg, bool& found) const {
    found = false;
    bool guard_seen = false;
    u32 count = 0;
    if (site.cond == arm::Cond::Ls) guard_seen = true, count = 1;  // (ARM: the table branch is itself conditional)
    else if (site.cond == arm::Cond::Cc) guard_seen = true, count = 0;
    auto it = t.code.find(site.address);
    for (int steps = 0; steps < 24 && it != t.code.begin(); ++steps) {
        --it;
        const Instruction& in = it->second;
        const Flow f = flow_of(in);
        if (in.op == Op::B && is_conditional(in) && !guard_seen) {
            if (in.cond == arm::Cond::Hi) guard_seen = true, count = 1;
            else if (in.cond == arm::Cond::Cs) guard_seen = true, count = 0;
            continue;
        }
        if (in.op == Op::Cmp && in.operands[0].kind == OperandKind::Reg && in.operands[0].reg == index_reg &&
            in.operands[1].kind == OperandKind::Imm) {
            if (!guard_seen) return 0;
            found = true;
            return in.operands[1].imm + count;
        }
        if (f.kind != FlowKind::Next && !(f.kind == FlowKind::Branch && f.conditional)) break;
    }
    return 0;
}

bool Analyzer::jump_table(Traversal& t, const Instruction& in, JumpTable& out, bool unbounded) {
    out = JumpTable{};
    out.site = in.address;
    const auto add_target = [&](u32 code) { out.targets.push_back(code); };
    if (in.op == Op::Tbb || in.op == Op::Tbh) {
        const auto& m = in.operands[0];
        if (m.reg != arm::kPc) return false;  // (a table elsewhere: not recognized)
        out.table = in.address + 4;
        out.index_reg = m.reg2;
        bool found;
        u32 count = bound_for(t, in, m.reg2, found);
        out.bounded = found;
        const u32 entry_size = in.op == Op::Tbb ? 1 : 2;
        if (!found && !unbounded) return false;
        // Without a bound: entries up to the first target (the table lies before the code it jumps to) or the first
        // code known after the table, whichever comes first.
        u32 limit = found ? count : 1024;
        u32 lowest = 0xFFFFFFFFu;
        if (!found)
            if (auto next = t.code.lower_bound(out.table); next != t.code.end()) lowest = next->first;
        for (u32 i = 0; i < limit; ++i) {
            const u32 at = out.table + i * entry_size;
            if (!found && at + entry_size > lowest) break;
            if (!image_.contains(at, entry_size)) return false;
            const u32 e = entry_size == 1 ? image_.u8_at(at) : image_.u16_at(at);
            const u32 target = out.table + 2 * e;
            lowest = std::min(lowest, target);
            add_target(target | 1u);
        }
        return !out.targets.empty();
    }
    // ldr pc, [pc, rX, lsl #2] (a table of addresses after the instruction, ARM or Thumb)
    if (in.op == Op::Ldr && in.operands[1].kind == OperandKind::Mem) {
        const auto& m = in.operands[1];
        if (m.reg != arm::kPc || !(m.flags & arm::kMemIndexReg) || m.shift != arm::ShiftType::Lsl || m.amount != 2 ||
            (m.flags & arm::kMemSubtract) || !(m.flags & arm::kMemPre) || (m.flags & arm::kMemWriteBack))
            return false;
        out.table = (pc_value(in)) & ~3u;
        out.index_reg = m.reg2;
        bool found;
        const u32 count = bound_for(t, in, m.reg2, found);
        if (!found) return false;
        out.bounded = true;
        for (u32 i = 0; i < count; ++i) {
            const u32 at = out.table + 4 * i;
            if (!image_.contains(at, 4)) return false;
            add_target(image_.u32_at(at));
        }
        return !out.targets.empty();
    }
    // add pc, pc, rX, lsl #2 (ARM: a table of branches after the instruction)
    if (in.op == Op::Add && in.mode == Mode::Arm && in.operand_count == 3 && in.operands[1].kind == OperandKind::Reg &&
        in.operands[1].reg == arm::kPc && in.operands[2].kind == OperandKind::RegShiftImm &&
        in.operands[2].shift == arm::ShiftType::Lsl && in.operands[2].amount == 2) {
        out.table = in.address + 8;
        out.index_reg = in.operands[2].reg;
        bool found;
        const u32 count = bound_for(t, in, out.index_reg, found);
        if (!found) return false;
        out.bounded = true;
        for (u32 i = 0; i < count; ++i) add_target(out.table + 4 * i);
        return true;
    }
    return false;
}

Function Analyzer::traverse(u32 entry) {
    Traversal t;
    t.fn.entry = entry;
    t.fn.mode = code_mode(entry);
    const Mode mode = t.fn.mode;
    const u32 start = entry & ~1u;
    const u32 bit = mode == Mode::Thumb ? 1u : 0u;
    nx::vector<u32> work{start};
    const auto report = [&](Severity s, u32 at, nx::string message) {
        if (collect_diagnostics_) diag(s, at, entry, std::move(message));
    };
    const auto branch_to = [&](const Instruction& from, u32 target_code) {
        // A branch to another function's entry is a tail call; anything else is inside this function.
        if (target_code != entry && (is_entry(target_code) || p_.import_stubs.contains(target_code & ~1u))) {
            t.tail_targets.push_back(target_code);
            return;
        }
        if (code_mode(target_code) != mode) {
            report(Severity::Error, from.address, nx::format("branch to 0x{:08x} changes instruction set", target_code));
            return;
        }
        t.labels.insert(target_code & ~1u);
        work.push_back(target_code & ~1u);
    };
    const auto take_table = [&](const Instruction& in, JumpTable& table) {
        bool all_ok = true;
        for (u32 target : table.targets) {
            if (code_mode(target) != mode || !image_.executable(target & ~1u)) {
                report(Severity::Error, in.address, nx::format("jump table entry 0x{:08x} is not code", target));
                all_ok = false;
            }
        }
        if (!all_ok) return;
        if (!table.bounded) report(Severity::Warning, in.address, "jump table without a bounds check: size inferred");
        for (u32 target : table.targets) branch_to(in, target);
        t.fn.tables[in.address] = std::move(table);
    };
    const auto drain = [&] {
        while (!work.empty()) {
            u32 a = work.back();
            work.pop_back();
            if (t.code.contains(a)) continue;
            arm::ItState it;
            u32 previous = 0;
            bool has_previous = false;
            for (;;) {
                if (has_previous) {
                    const u32 code = a | bit;
                    if (code != entry && (is_entry(code) || p_.import_stubs.contains(a))) {
                        t.fn.runs_into[previous] = p_.import_stubs.contains(a) ? a : code;
                        t.tail_targets.push_back(code);
                        break;
                    }
                    if (t.code.contains(a)) break;  // joins code already decoded
                }
                // An address inside an instruction already decoded: overlapping code.
                if (auto next = t.code.lower_bound(a); next != t.code.begin()) {
                    auto prev = next;
                    --prev;
                    if (prev->first < a && prev->first + prev->second.size > a) {
                        report(Severity::Error, a, nx::format("code at 0x{:08x} overlaps the instruction at 0x{:08x}", a, prev->first));
                        break;
                    }
                }
                bool ok;
                Instruction in = decode_at(a, mode, it, ok);
                if (!ok) {
                    report(Severity::Error, a, nx::format("control reaches 0x{:08x}, outside the executable segment", a));
                    // (recorded as an undefined instruction so the generated code traps there)
                    in = Instruction{};
                    in.address = a;
                    in.mode = mode;
                    in.size = mode == Mode::Arm ? 4 : 2;
                    t.code[a] = in;
                    break;
                }
                if (auto next = t.code.lower_bound(a + 1); next != t.code.end() && next->first < a + in.size) {
                    report(Severity::Error, a, nx::format("the instruction at 0x{:08x} overlaps code at 0x{:08x}", a, next->first));
                    break;
                }
                if (!in.valid()) report(Severity::Error, a, nx::format("undefined instruction at 0x{:08x}", a));
                else if (in.flags & arm::kUnpredictable)
                    report(Severity::Error, a, nx::format("unpredictable instruction at 0x{:08x}: {}", a, arm::format(in)));
                t.code[a] = in;
                const Flow f = flow_of(in);
                bool stop = false;
                switch (f.kind) {
                    case FlowKind::Next: break;
                    case FlowKind::Trap: stop = !f.conditional; break;
                    case FlowKind::Branch:
                        branch_to(in, f.target);
                        stop = !f.conditional;
                        break;
                    case FlowKind::Call:
                        add_entry(f.target, entry, "call target");
                        if (is_noreturn(f.target)) {
                            t.fn.noreturn_calls.push_back(a);
                            stop = !f.conditional;
                        }
                        break;
                    case FlowKind::IndirectCall: break;
                    case FlowKind::IndirectBranch: {
                        JumpTable table;
                        if (jump_table(t, in, table, false)) take_table(in, table);
                        else if (in.op == Op::Tbb || in.op == Op::Tbh) t.deferred.push_back(in);
                        else t.may_return = true;
                        stop = !f.conditional;
                        break;
                    }
                }
                if (stop) break;
                previous = a;
                has_previous = true;
                a += in.size;
            }
        }
    };
    drain();
    // Table branches without a bounds check, now that the code after their tables is known (one at a time: each
    // table's targets may bound the next).
    while (!t.deferred.empty()) {
        const Instruction in = t.deferred.back();
        t.deferred.pop_back();
        JumpTable table;
        if (jump_table(t, in, table, true)) {
            take_table(in, table);
        } else {
            report(Severity::Error, in.address, "table branch whose table was not found");
            t.may_return = true;
        }
        drain();
    }
    exits_[entry] = Exits{t.may_return, std::move(t.tail_targets)};
    t.fn.code.reserve(t.code.size());
    for (auto& [address, in] : t.code) t.fn.code.push_back(in);
    t.fn.labels.assign(t.labels.begin(), t.labels.end());
    std::sort(t.fn.labels.begin(), t.fn.labels.end());
    return std::move(t.fn);
}

bool Analyzer::update_noreturn() {
    // Least fixed point of "returns": a function returns when it reaches a return, or ends in (a tail call to, or
    // running into) a function or import that returns. Functions not traversed yet are assumed to return.
    nx::flat_hash_set<u32> returns;
    for (const auto& [code, e] : exits_)
        if (e.may_return) returns.insert(code);
    const auto target_returns = [&](u32 target) {
        if (auto stub = p_.import_stubs.find(target & ~1u); stub != p_.import_stubs.end())
            return !noreturn_imports_.contains(stub->second);
        if (!exits_.contains(target)) return true;
        return returns.contains(target);
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (const auto& [code, e] : exits_) {
            if (returns.contains(code)) continue;
            for (u32 target : e.tail_targets)
                if (target_returns(target)) {
                    returns.insert(code);
                    changed = true;
                    break;
                }
        }
    }
    bool changed = false;
    for (const auto& [code, e] : exits_)
        if (!returns.contains(code) && noreturn_.insert(code).second) changed = true;
    return changed;
}

void Analyzer::run() {
    const auto& m = image_.module();
    for (const auto& lib : m.imports)
        for (const auto& f : lib.functions) {
            p_.import_stubs[f.address & ~1u] = static_cast<u32>(p_.imports.size());
            p_.imports.push_back(Program::ImportRef{lib.nid, f.nid, lib.name});
        }
    {
        nx::vector<Image::ImportedVariable> variables;
        auto uses = image_.variable_uses(&variables);
        if (!uses) diag(Severity::Error, 0, 0, uses.error().message);
        else
            for (const auto& u : *uses) p_.variable_uses[u.at] = u;
        for (const auto& v : variables) p_.variable_imports.push_back(Program::ImportRef{v.library_nid, v.nid, v.library});
    }
    // The module's own export gives module_start/stop/exit as code addresses. (The module information has their
    // offsets too, but without the instruction set.)
    for (const auto& lib : m.exports)
        for (const auto& f : lib.functions) {
            if (lib.attributes & module::kExportMain) {
                if (f.nid != module::kNidModuleStart && f.nid != module::kNidModuleStop && f.nid != module::kNidModuleExit)
                    continue;  // (module_info, proc_param, sdk_version: data)
            }
            if (!image_.executable(f.address & ~1u)) {
                // (libstdc++ exports std::defer_lock and its kin, empty objects, as functions)
                diag(Severity::Note, f.address, 0, nx::format("exported function 0x{:08x} is not in an executable segment: not code", f.address));
                continue;
            }
            add_entry(f.address, 0, "exported function");
        }
    if (options_.use_exidx)
        for (u32 code : exidx_entries(image_)) {
            // An entry's address has bit 0 set for Thumb code. Even ones (ARM) are taken only from other evidence:
            // in SDK-built modules they are the import stubs and the end-of-code sentinel.
            if (code & 1) add_entry(code, 0, "exception index entry");
        }
    for (u32 code : options_.extra_entries) add_entry(code, 0, "extra entry");
    {
        nx::flat_hash_set<u32> nids;
        for (u32 nid : options_.noreturn_imports) nids.insert(nid);
        for (usize i = 0; i < p_.imports.size(); ++i)
            if (nids.contains(p_.imports[i].nid)) noreturn_imports_.insert(static_cast<u32>(i));
    }

    // Passes until neither new entries nor new no-return functions turn up; the last pass rebuilds every function
    // with the final sets.
    for (;;) {
        const usize before = entries_.size();
        nx::vector<u32> todo(entries_.begin(), entries_.end());
        pending_.clear();
        for (u32 code : todo) traverse(code);
        while (!pending_.empty()) {
            nx::vector<u32> more = std::move(pending_);
            pending_.clear();
            for (u32 code : more) traverse(code);
        }
        const bool noreturn_changed = update_noreturn();
        if (entries_.size() == before && !noreturn_changed) break;
    }
    collect_diagnostics_ = true;
    nx::vector<u32> all(entries_.begin(), entries_.end());
    std::sort(all.begin(), all.end());
    for (u32 code : all) {
        Function fn = traverse(code);
        fn.returns = !noreturn_.contains(code);
        p_.functions[code] = std::move(fn);
    }
    // (diagnostics from discovery passes repeat in the final pass; only the final pass reports)
}

}  // namespace

Program analyze(const Image& image, const Options& options) {
    Program p;
    p.image = &image;
    Analyzer(image, options, p).run();
    return p;
}

}  // namespace psvrecore::analysis
