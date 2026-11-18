#include "psvrecore/recomp/generator.hpp"

#include <algorithm>

#include "emit.hpp"
#include "psvrecore/arm/format.hpp"

namespace psvrecore::recomp {

using namespace detail;

nx::string function_name(u32 code) { return nx::format("f_{:08X}", code & ~1u); }

namespace detail {

nx::string condition(arm::Cond c) {
    switch (c) {
        case arm::Cond::Eq: return "ctx->z";
        case arm::Cond::Ne: return "!ctx->z";
        case arm::Cond::Cs: return "ctx->c";
        case arm::Cond::Cc: return "!ctx->c";
        case arm::Cond::Mi: return "ctx->n";
        case arm::Cond::Pl: return "!ctx->n";
        case arm::Cond::Vs: return "ctx->v";
        case arm::Cond::Vc: return "!ctx->v";
        case arm::Cond::Hi: return "(ctx->c & !ctx->z)";
        case arm::Cond::Ls: return "(!ctx->c | ctx->z)";
        case arm::Cond::Ge: return "ctx->n == ctx->v";
        case arm::Cond::Lt: return "ctx->n != ctx->v";
        case arm::Cond::Gt: return "(!ctx->z & (ctx->n == ctx->v))";
        case arm::Cond::Le: return "(ctx->z | (ctx->n != ctx->v))";
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

bool identifier_char(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

// A function's body with the condition flags (N, Z, C, V) in a local, `fl`, instead of the context: the C compiler
// then drops the flags no instruction reads (stores to the context it cannot drop: the guest's memory accesses
// between them are atomic). They are written to the context before whatever may read them there (a call, a jump, an
// import, a system call, a trap, a return, MRS and MSR) and read back after what returns here.
nx::string localize_flags(nx::string_view body) {
    auto replaced = [](nx::string_view line) {
        nx::string out;
        for (usize i = 0; i < line.size();) {
            if (line.substr(i, 7) == "psv_nz(" && line.substr(i + 7, 4) == "ctx,") {
                out += "psv_nz_fl(&fl,", i += 11;
                continue;
            }
            if (line.substr(i, 9) == "psv_adds(" && line.substr(i + 9, 4) == "ctx,") {
                out += "psv_adds_fl(&fl,", i += 13;
                continue;
            }
            if (line.substr(i, 5) == "ctx->" && i + 6 <= line.size() && nx::string_view("nzcv").find(line[i + 5]) != nx::string_view::npos &&
                (i + 6 == line.size() || !identifier_char(line[i + 6])) && (i == 0 || !identifier_char(line[i - 1]))) {
                out += "fl.", out += line[i + 5], i += 6;
                continue;
            }
            out += line[i++];
        }
        return out;
    };
    auto has = [](nx::string_view line, nx::string_view what) { return line.find(what) != nx::string_view::npos; };
    nx::string out;
    usize at = 0;
    while (at < body.size()) {
        usize end = body.find('\n', at);
        if (end == nx::string_view::npos) end = body.size();
        nx::string line = replaced(body.substr(at, end - at));
        at = end + 1;
        // (a call to recompiled code, `f_XXXXXXXX(ctx, ...)`, or to the runtime)
        bool call = false;
        for (usize p = line.find("f_"); p != nx::string::npos; p = line.find("f_", p + 2))
            if ((p == 0 || !identifier_char(line[p - 1])) && p + 15 <= line.size() && line.substr(p + 10, 5) == "(ctx,") call = true;
        call |= has(line, "psv_call(") || has(line, "psv_call_import(") || has(line, "psv_jump(") || has(line, "psv_svc(") ||
                has(line, "psv_yield(") || has(line, "psv_trap(") || has(line, "psv_read_apsr(") || has(line, "psv_write_apsr(") ||
                has(line, "psv_import_missing(");
        // (each return written as one that stores the flags first)
        nx::string fixed;
        for (usize p = 0;;) {
            const usize r = line.find("return;", p);
            if (r == nx::string::npos) {
                fixed += line.substr(p);
                break;
            }
            fixed += line.substr(p, r - p);
            fixed += "{ PSV_FL_OUT(fl); return; }";
            p = r + 7;
        }
        if (call) {
            const usize indent = fixed.find_first_not_of(' ');
            const nx::string pad = fixed.substr(0, indent == nx::string::npos ? 0 : indent);
            const bool returns = has(line, "return;") || has(line, "psv_trap(");
            const nx::string_view rest = nx::string_view(fixed).substr(pad.size());
            if (rest.substr(0, 5) == "case " || rest.substr(0, 8) == "default:") {
                // (a jump table's case: stored after its label, where the switch lands)
                const usize colon = fixed.find(':');
                out += fixed.substr(0, colon + 1) + " PSV_FL_OUT(fl);" + fixed.substr(colon + 1) + "\n";
                if (!returns) out += pad + "PSV_FL_IN(fl);\n";
                continue;
            }
            out += pad + "PSV_FL_OUT(fl);\n" + fixed + "\n";
            if (!returns) out += pad + "PSV_FL_IN(fl);\n";
        } else {
            out += fixed + "\n";
        }
    }
    return out;
}

}  // namespace

void Emitter::trap(const Instruction& in, const char* kind, nx::string_view what) {
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

void Emitter::branch_to_value(const Instruction& in, nx::string_view target) {
    const nx::string t = temp("target");
    line(nx::format("const uint32_t {} = {};", t, target));
    if (auto table = fn_.tables.find(in.address); table != fn_.tables.end()) {
        ++stats_.jump_tables;
        // (the table is in read-only memory: its targets are known, so the jump is a switch on the target)
        line(nx::format("switch ({}) {{", t));
        nx::flat_hash_set<u32> seen;
        for (u32 target : table->second.targets) {
            if (!seen.insert(target).second) continue;
            const bool tail = target != fn_.entry && program_.function_at(target) != nullptr;
            if (tail) line(nx::format("case {}: {}(ctx, ret); return;", hex(target), function_name(target)));
            else line(nx::format("case {}: goto {};", hex(target), label_name(target)));
        }
        line(nx::format("default: psv_trap(ctx, {}, PSV_TRAP_BAD_JUMP);", hex(in.address)));
        line("}");
        return;
    }
    ++stats_.indirect_jumps;
    line(nx::format("if ({} == ret) return;", t));
    line(nx::format("psv_jump(ctx, {}, ret, {});", t, hex(in.address)));
    line("return;");
}

void Emitter::branch_to(const Instruction& in, u32 target_code) {
    (void)in;
    if (auto stub = program_.import_stubs.find(target_code & ~1u); stub != program_.import_stubs.end()) {
        ++stats_.import_calls;
        line(nx::format("psv_call_import(ctx, &{}_imports[{}], ret);", prefix_, stub->second));
        line("return;");
        return;
    }
    if (target_code != fn_.entry && program_.function_at(target_code)) {
        ++stats_.tail_calls;
        line(nx::format("{}(ctx, ret);", function_name(target_code)));
        line("return;");
        return;
    }
    line(nx::format("goto {};", label_name(target_code)));
}

void Emitter::call(const Instruction& in, u32 target_code) {
    const u32 next = next_code(in);
    line(nx::format("{} = {};", R(arm::kLr), hex(next)));
    if (auto stub = program_.import_stubs.find(target_code & ~1u); stub != program_.import_stubs.end()) {
        ++stats_.import_calls;
        line(nx::format("psv_call_import(ctx, &{}_imports[{}], {});", prefix_, stub->second, hex(next)));
        return;
    }
    if (program_.function_at(target_code)) {
        line(nx::format("{}(ctx, {});", function_name(target_code), hex(next)));
        return;
    }
    ++stats_.indirect_calls;
    line(nx::format("psv_call(ctx, {}, {});", hex(target_code), hex(next)));
}

bool Emitter::constant_load(u32 address, u32 size, u32& value) const {
    const auto& image = *program_.image;
    // (a word the loader patches with an imported variable's address is not known now)
    for (u32 at = address >= 3 ? address - 3 : 0; at < address + size; ++at)
        if (program_.variable_uses.contains(at)) return false;
    const auto* seg = image.segment_of(address);
    if (!seg || seg->writable() || !image.contains(address, size)) return false;
    value = size == 4 ? image.u32_at(address) : size == 2 ? image.u16_at(address) : image.u8_at(address);
    return true;
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
    if (emit_core(*this, in) || emit_memory(*this, in) || emit_vfp(*this, in) || emit_simd(*this, in)) return;
    unimplemented(in);
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
    nx::flat_hash_set<u32> noreturn(fn_.noreturn_calls.begin(), fn_.noreturn_calls.end());
    for (usize i = 0; i < code.size(); ++i) {
        const auto& in = code[i];
        const u32 next = in.address + in.size;
        if (falls_through(in) && !fn_.runs_into.contains(in.address) && !noreturn.contains(in.address) &&
            (i + 1 >= code.size() || code[i + 1].address != next) && addresses.contains(next))
            labels_.insert(next);
    }
    const bool entry_first = !code.empty() && code.front().address == fn_.start();
    if (!entry_first) labels_.insert(fn_.start());

    nx::string out;
    if (!entry_first) out += nx::format("    goto {};\n", label_name(fn_.start()));
    for (usize i = 0; i < code.size(); ++i) {
        const auto& in = code[i];
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
        if (!falls_through(in) || ended_) continue;
        const u32 next = in.address + in.size;
        if (auto into = fn_.runs_into.find(in.address); into != fn_.runs_into.end()) {
            body_.clear();
            branch_to(in, into->second);  // (a code address, or an import stub's address)
            out += "    /* runs into the next function */\n" + body_;
            continue;
        }
        if (noreturn.contains(in.address)) {
            out += nx::format("    psv_trap(ctx, {}, PSV_TRAP_RETURNED);\n", hex(next));
            continue;
        }
        if (i + 1 < code.size() && code[i + 1].address == next) continue;
        if (addresses.contains(next)) out += nx::format("    goto {};\n", label_name(next));
        else out += nx::format("    psv_trap(ctx, {}, PSV_TRAP_UNDISCOVERED);\n", hex(next));
    }
    ++stats_.functions;
    return nx::format("\nvoid {}(PsvContext* PSV_RESTRICT ctx, uint32_t ret) {{\n    PsvFlags fl;\n    PSV_FL_IN(fl);\n",
                      function_name(fn_.entry)) +
           localize_flags(out) + "}\n";
}

}  // namespace detail

namespace {

nx::string module_source(const analysis::Program& program, const Options& options) {
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
    out += nx::format("const PsvModuleCode {}_module = {{\"{}\", {}_function_table, {}u, {}_imports, {}u, {}_import_stubs, {}_variables, {}u,\n"
                      "    {{{}}}, {}_segment_bases, {}_segment_sizes, {}u}};\n",
                      p, program.image->module().info.name, p, program.functions.size(), p, program.imports.size(), p, p,
                      program.variable_imports.size(), fingerprint, p, p, segments.size());
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
    for (const auto& [code, fn] : program.functions) {
        Emitter e(program, fn, p, result.stats);
        chunk += e.function();
        chunk_instructions += static_cast<u32>(fn.code.size());
        if (chunk_instructions >= options.instructions_per_file) flush();
    }
    flush();
    result.files.push_back(GeneratedFile{p + "_module.c", module_source(program, options)});
    return result;
}

}  // namespace psvrecore::recomp
