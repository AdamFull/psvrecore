#include "psvrecore/recomp/generator.hpp"

#include <algorithm>
#include <utility>

#include "emit.hpp"
#include "psvrecore/arm/format.hpp"

namespace psvrecore::recomp {

using namespace detail;

nx::string function_name(u32 code) { return nx::format("f_{:08X}", code & ~1u); }

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

}  // namespace

Emitter::Emitter(const analysis::Program& program, const analysis::Function& fn, const analysis::FlagLiveness& flags,
                 const nx::string& prefix, Stats& stats)
    : program_(program), fn_(fn), flags_(flags), flags_fn_(flags.at(fn.entry)), cfg_(analysis::build_cfg(program, fn)),
      prefix_(prefix), stats_(stats) {}

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
    store_flags_for_trap();
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

nx::string Emitter::read(const Instruction& in, u32 r) const {
    if (r == arm::kPc) return hex(analysis::pc_value(in));
    return R(r);
}

void Emitter::write(const Instruction& in, u32 r, nx::string_view value, PcWrite kind) {
    if (r != arm::kPc) {
        line(nx::format("{} = {};", R(r), value));
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
        store_flags_for_trap();
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
        line(nx::format("uint32_t {} = {} == {} ? {} : 0xFFFFFFFFu;", index, R(table.base_reg), hex(table.table), read(in, table.index_reg)));
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
    line("        " + flags_line(analysis::kAllFlags, true));
    line(nx::format("        psv_trap(ctx, {}, PSV_TRAP_BAD_JUMP);", hex(in.address)));
    line("    }");
    line("}");
    line(nx::format("switch ({}) {{", index));
    for (u32 i = 0; i < n; ++i) {
        const u32 target_code = targets[i];
        const bool tail = target_code != fn_.entry && program_.function_at(target_code) != nullptr;
        if (tail) {
            const nx::string store = flags_line(flags_.into(target_code, flags_fn_->live_out), true);
            line(nx::format("case {}u: {}{}{}(ctx, ret); return;", i, store, store.empty() ? "" : " ", function_name(target_code)));
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
    store_flags(out);
    line(nx::format("if ({} == ret) return;", t));
    store_flags(static_cast<FlagSet>(jump & ~out));
    line(nx::format("psv_jump(ctx, {}, ret, {});", t, hex(in.address)));
    line("return;");
}

void Emitter::branch_to(const Instruction& in, u32 target_code) {
    (void)in;
    if (auto stub = program_.import_stubs.find(target_code & ~1u); stub != program_.import_stubs.end()) {
        ++stats_.import_calls;
        store_flags(flags_.into_unknown(flags_fn_->live_out));
        line(nx::format("psv_call_import(ctx, &{}_imports[{}], ret);", prefix_, stub->second));
        line("return;");
        return;
    }
    if (target_code != fn_.entry && program_.function_at(target_code)) {
        ++stats_.tail_calls;
        store_flags(flags_.into(target_code, flags_fn_->live_out));
        line(nx::format("{}(ctx, ret);", function_name(target_code)));
        line("return;");
        return;
    }
    line(nx::format("goto {};", label_name(target_code)));
}

void Emitter::call(const Instruction& in, u32 target_code) {
    const u32 next = next_code(in);
    line(nx::format("{} = {};", R(arm::kLr), hex(next)));
    call_flags_before();
    if (auto stub = program_.import_stubs.find(target_code & ~1u); stub != program_.import_stubs.end()) {
        ++stats_.import_calls;
        line(nx::format("psv_call_import(ctx, &{}_imports[{}], {});", prefix_, stub->second, hex(next)));
    } else if (program_.function_at(target_code)) {
        line(nx::format("{}(ctx, {});", function_name(target_code), hex(next)));
    } else {
        ++stats_.indirect_calls;
        line(nx::format("psv_call(ctx, {}, {});", hex(target_code), hex(next)));
    }
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
    if (std::find(fn_.noreturn_calls.begin(), fn_.noreturn_calls.end(), in.address) != fn_.noreturn_calls.end()) {
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

void Emitter::instruction(const Instruction& in) {
    ended_ = false;
    if (!in.valid()) return trap(in, "PSV_TRAP_UNDEFINED", "undefined");
    if (in.flags & arm::kUnpredictable) return trap(in, "PSV_TRAP_UNPREDICTABLE", nx::format("unpredictable {}", arm::mnemonic(in.op)));
    count_flags(in);
    if (emit_core(*this, in) || emit_memory(*this, in) || emit_vfp(*this, in) || emit_simd(*this, in)) return;
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
        return nx::format("\nvoid {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret) {{ (void)ctx; (void)ret; }}\n", function_name(fn_.entry));
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
    for (usize i = 0; i < code.size(); ++i) {
        const auto& in = code[i];
        index_ = i;
        ++stats_.instructions;
        if (labels_.contains(in.address)) out += nx::format("{}:;\n", label_name(in.address));
        body_.clear();
        temp_counter_ = 0;
        instruction(in);
        // (an IT instruction's condition is its block's first condition, not its own)
        const nx::string cond = in.op == Op::It ? nx::string() : condition(in.cond);
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
            branch_to(in, into->second);  // (a code address, or an import stub's address)
            out += "    /* runs into the next function */\n" + body_;
            continue;
        }
        if (i + 1 < code.size() && code[i + 1].address == next) continue;
        if (addresses.contains(next)) {
            out += nx::format("    goto {};\n", label_name(next));
        } else {
            out += "    " + flags_line(analysis::kAllFlags, true) + "\n";
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
    return nx::format("\nvoid {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret) {{\n{}    PsvFlags fl = {{{}}};\n", function_name(fn_.entry),
                      memory ? "    uint8_t* const mb = psv_membase;\n" : "", flags) +
           out + "}\n";
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
    // (the condition flags' contract at the module's boundaries: guest.h, PsvModuleCode)
    out += nx::format("const PsvModuleCode {}_module = {{\"{}\", {}_function_table, {}u, {}_imports, {}u, {}_import_stubs, {}_variables, {}u,\n"
                      "    {{{}}}, {}_segment_bases, {}_segment_sizes, {}u,\n    PSV_FLAG_CONTRACT, 0x{:X}u, 0x{:X}u, 0x{:X}u, 0x{:X}u}};\n",
                      p, program.image->module().info.name, p, program.functions.size(), p, program.imports.size(), p, p,
                      program.variable_imports.size(), fingerprint, p, p, segments.size(), flags.needed_at_entry, flags.given_at_entry,
                      flags.needed_at_return, flags.given_at_return);
    return out;
}

}  // namespace

Output generate(const analysis::Program& program, const Options& options) {
    Output result;
    const nx::string& p = options.name;
    nx::string header = nx::format(
        "/* Generated by psvrecomp: module {}. */\n#pragma once\n#include \"psvrecore/recomp/guest.h\"\n\n"
        "extern PsvImport {}_imports[];\nextern PsvVariable {}_variables[];\nextern const PsvModuleCode {}_module;\n\n",
        program.image->module().info.name, p, p, p);
    for (const auto& [code, fn] : program.functions) header += nx::format("void {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret);\n", function_name(code));
    result.files.push_back(GeneratedFile{p + "_functions.h", std::move(header)});

    nx::string chunk;
    u32 chunk_instructions = 0, chunk_index = 0;
    const auto flush = [&] {
        if (chunk.empty()) return;
        result.files.push_back(GeneratedFile{nx::format("{}_code_{}.c", p, chunk_index++),
                                             nx::format("/* Generated by psvrecomp. */\n#include \"{}_functions.h\"\n", p) + chunk});
        chunk.clear();
        chunk_instructions = 0;
    };
    analysis::FlagOptions flag_options;
    flag_options.everywhere = options.flags_everywhere;
    flag_options.given_at_entry = options.flags_given_at_entry;
    flag_options.given_at_return = options.flags_given_at_return;
    flag_options.full_exit = options.full_exit_flags;
    analysis::FlagLiveness flags = analysis::flag_liveness(program, flag_options);
    for (const auto& [code, fn] : program.functions) {
        Emitter e(program, fn, flags, p, result.stats);
        chunk += e.function();
        chunk_instructions += static_cast<u32>(fn.code.size());
        if (chunk_instructions >= options.instructions_per_file) flush();
    }
    flush();
    result.files.push_back(GeneratedFile{p + "_module.c", module_source(program, options, flags)});
    // (the summaries only: the per-instruction sets are the generator's business)
    for (auto& [code, f] : flags.functions) {
        nx::vector<FlagSet>().swap(f.after);
        nx::vector<FlagSet>().swap(f.after_call);
        nx::vector<FlagSet>().swap(f.before_call);
    }
    result.flags = std::move(flags);
    return result;
}

}  // namespace psvrecore::recomp
