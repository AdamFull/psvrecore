// corpusgen — translates the synthetic corpus (corpus.hpp) with the code generator, as psvrecomp translates a
// module: code discovery from the modules' function symbols (no exception index), then the generator.
//
//   corpusgen <out-dir> [instructions-per-file] [--full-exit] [--no-callee-saved] [--registers-in-context]
//             [--keep-registers all|loops|none] [--no-sp-check] [--vfp-in-context] [--vfp-calls-conservative]
//             [--neon-in-context] [--no-q-locals]
//
// Output: <out-dir>/syn_<module>/syn_<module>_{functions.h,code_<k>.c,module.c} for each module (the layout of a
// game's generated/code folder), <out-dir>/corpus_modules.c, the table of the modules' PsvModuleCode and of each
// function's condition flags at return (those its returns leave in the context: what the test compares), and
// <out-dir>/corpus_functions.tsv, each function's C name and source name (for scripts/codegen_metrics.py).
//
// The modules are generated as a game's are, each given what the others read (psvrecomp --flags-given-at-entry and
// --flags-given-at-return). --full-exit: the functions the test runs leave all four flags at return, as exported
// functions do, so that the test compares every flag at exit. --no-callee-saved and --registers-in-context: as
// psvrecomp's (for measuring the register stages apart, and for chasing a register analysis bug).

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "corpus.hpp"
#include "psvrecore/analysis/flags.hpp"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/recomp/generator.hpp"
#include "core/foundation/strings/format.h"

using namespace psvrecore;

namespace {

analysis::Image image_of(const CorpusModule& m) {
    analysis::Segment seg;
    seg.base = seg.linked = m.base;
    seg.flags = elf::kSegmentRead | elf::kSegmentExecute;
    seg.bytes.assign(m.bytes, m.bytes + m.size);
    seg.file_size = seg.size();
    return analysis::Image::from_segments({seg});
}

analysis::Program discover(const analysis::Image& image, const CorpusModule& m) {
    analysis::Options options;
    options.use_exidx = false;
    for (u32 i = 0; i < m.function_count; ++i) options.extra_entries.push_back(m.functions[i].address);
    return analysis::analyze(image, options);
}

}  // namespace

int main(int argc, char** argv) {
    bool full_exit = false, no_callee_saved = false, registers_in_context = false;
    nx::string keep = "all";
    bool no_sp_check = false, vfp_in_context = false, vfp_calls_conservative = false, neon_in_context = false, no_q_locals = false;
    bool no_internal_calls = false, no_call_table = false, inline_in_file = false, call_table_inline = false;
    u32 inline_limit = recomp::Options{}.inline_limit;
    nx::vector<const char*> args;
    u32 split = 0;  // (--split K: each module's code in exactly K files, the last ones empty when it has less)
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--full-exit") == 0) full_exit = true;
        else if (std::strcmp(argv[i], "--no-callee-saved") == 0) no_callee_saved = true;
        else if (std::strcmp(argv[i], "--registers-in-context") == 0) registers_in_context = true;
        else if (std::strcmp(argv[i], "--keep-registers") == 0 && i + 1 < argc) keep = argv[++i];
        else if (std::strcmp(argv[i], "--no-sp-check") == 0) no_sp_check = true;
        else if (std::strcmp(argv[i], "--vfp-in-context") == 0) vfp_in_context = true;
        else if (std::strcmp(argv[i], "--vfp-calls-conservative") == 0) vfp_calls_conservative = true;
        else if (std::strcmp(argv[i], "--neon-in-context") == 0) neon_in_context = true;
        else if (std::strcmp(argv[i], "--no-q-locals") == 0) no_q_locals = true;
        else if (std::strcmp(argv[i], "--no-internal-calls") == 0) no_internal_calls = true;
        else if (std::strcmp(argv[i], "--no-call-table") == 0) no_call_table = true;
        else if (std::strcmp(argv[i], "--call-table-inline") == 0) call_table_inline = true;
        else if (std::strcmp(argv[i], "--inline-in-file") == 0) inline_in_file = true;
        else if (std::strcmp(argv[i], "--inline-limit") == 0 && i + 1 < argc) inline_limit = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
        else if (std::strcmp(argv[i], "--per-file") == 0 && i + 1 < argc) ++i;
        else if (std::strcmp(argv[i], "--split") == 0 && i + 1 < argc) split = static_cast<u32>(std::strtoul(argv[++i], nullptr, 0));
        else args.push_back(argv[i]);
    }
    if (args.size() != 1 && args.size() != 2) {
        std::fprintf(stderr, "usage: corpusgen <out-dir> [instructions-per-file] [--full-exit] [--no-callee-saved] [--registers-in-context]\n");
        return 2;
    }
    const nx::fs::path_view dir(args[0]);
    u32 per_file = args.size() == 2 ? static_cast<u32>(std::strtoul(args[1], nullptr, 0)) : recomp::Options{}.instructions_per_file;
    // (--per-file N: instead of the positional one, for a build's PSVRECORE_CODEGEN_TEST_ARGS: functions in files of
    // their own, to measure calls across files)
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], "--per-file") == 0) per_file = static_cast<u32>(std::strtoul(argv[i + 1], nullptr, 0));
    if (per_file == 0) {
        std::fprintf(stderr, "corpusgen: instructions-per-file must be at least 1\n");
        return 2;
    }
    // What the modules read at entry and after indirect calls, each module's flags given to all (see above).
    analysis::FlagSet needed_at_entry = 0, needed_at_return = 0;
    const auto full_exit_of = [&](const CorpusModule& m) {
        nx::vector<u32> out;
        if (full_exit)
            for (const CorpusTest& t : kCorpusTests)
                if (std::strcmp(t.module, m.name) == 0) out.push_back(t.address);
        return out;
    };
    // (what a module reads can depend on what it is given: what the flags given at entry make live; to a fixed point)
    for (bool changed = true; changed;) {
        changed = false;
        for (const CorpusModule& m : kCorpusModules) {
            const auto image = image_of(m);
            analysis::FlagOptions options;
            options.full_exit = full_exit_of(m);
            options.given_at_entry = needed_at_entry;
            options.given_at_return = needed_at_return;
            const auto flags = analysis::flag_liveness(discover(image, m), options);
            if ((flags.needed_at_entry & ~needed_at_entry) || (flags.needed_at_return & ~needed_at_return)) changed = true;
            needed_at_entry |= flags.needed_at_entry;
            needed_at_return |= flags.needed_at_return;
        }
    }
    const auto write = [](nx::string_view path, nx::string_view text) {
        if (auto ok = io::write_text_file(nx::fs::path_view(path), text); !ok) {
            std::fprintf(stderr, "corpusgen: %s\n", ok.error().message.c_str());
            std::exit(1);
        }
    };
    nx::string table = "/* Generated by corpusgen. */\n#include \"psvrecore/recomp/guest.h\"\n\n";
    nx::string list, exits, names = "module\tfunction\tsymbol\n";
    for (const CorpusModule& m : kCorpusModules) {
        const auto image = image_of(m);
        const auto program = discover(image, m);
        usize errors = 0;
        for (const auto& d : program.diagnostics)
            if (d.severity == analysis::Severity::Error) {
                std::fprintf(stderr, "corpusgen: %s: 0x%08x: %s\n", m.name, d.address, d.message.c_str());
                ++errors;
            }
        recomp::Options gen;
        gen.name = nx::format("syn_{}", m.name);
        gen.instructions_per_file = per_file;
        if (split) {
            u32 total = 0;
            for (const auto& [code, fn] : program.functions) total += static_cast<u32>(fn.code.size());
            gen.instructions_per_file = std::max<u32>(1, (total + split - 1) / split);
        }
        gen.flags_given_at_entry = needed_at_entry;
        gen.flags_given_at_return = needed_at_return;
        gen.full_exit_flags = full_exit_of(m);
        gen.no_callee_saved = no_callee_saved;
        gen.registers_in_context = registers_in_context;
        gen.check_sp = !no_sp_check;
        gen.vfp_in_context = vfp_in_context;
        gen.vfp_calls_conservative = vfp_calls_conservative;
        gen.neon_in_context = neon_in_context;
        gen.no_q_locals = no_q_locals;
        gen.internal_calls = !no_internal_calls;
        gen.call_table = !no_call_table;
        gen.call_table_inline = call_table_inline;
        gen.inline_limit = inline_limit;
        gen.noinline_internal = !inline_in_file;
        gen.keep_registers = keep == "loops" ? analysis::KeepPolicy::Loops : keep == "none" ? analysis::KeepPolicy::None : analysis::KeepPolicy::All;
        const auto output = recomp::generate(program, gen);
        if (output.stats.register_effect_mismatches) {
            for (const auto& note : output.stats.register_notes) std::fprintf(stderr, "corpusgen: %s: %s\n", m.name, note.c_str());
            ++errors;  // (the analysis' model of what each instruction reads and writes must be the translation's)
        }
        for (const auto& [code, f] : output.flags.functions) exits += nx::format("    {{0x{:08X}u, 0x{:X}u}},\n", code, f.live_out);
        for (const auto& [what, n] : output.stats.traps) {
            std::fprintf(stderr, "corpusgen: %s: %zu x %s\n", m.name, static_cast<size_t>(n), what.c_str());
            ++errors;
        }
        if (errors) {
            std::fprintf(stderr, "corpusgen: %s: discovery errors or untranslated instructions\n", m.name);
            return 1;
        }
        const nx::string sub = dir / nx::fs::path_view(gen.name);
        if (!nx::fs::create_directories(nx::fs::path_view(sub))) {
            std::fprintf(stderr, "corpusgen: cannot create %s\n", sub.c_str());
            return 1;
        }
        for (const auto& f : output.files) write(nx::fs::path_view(sub) / nx::fs::path_view(f.name), f.text);
        usize chunks = 0;
        for (const auto& f : output.files) chunks += f.name.find("_code_") != nx::string::npos;
        if (split && chunks > split) {
            std::fprintf(stderr, "corpusgen: %s: %zu files, more than --split %u\n", m.name, chunks, split);
            return 1;
        }
        for (usize k = chunks; k < split; ++k)
            write(nx::fs::path_view(sub) / nx::fs::path_view(nx::format("{}_code_{}.c", gen.name, k)), "/* Generated by corpusgen: no code. */\n");
        for (u32 i = 0; i < m.function_count; ++i)
            names += nx::format("{}\t{}\t{}\n", gen.name, recomp::function_name(m.functions[i].address), m.functions[i].name);
        table += nx::format("extern const PsvModuleCode {}_module;\n", gen.name);
        list += nx::format("    &{}_module,\n", gen.name);
        std::printf("%s\n", nx::format("corpusgen: {}: {} functions, {} instructions, {} files", gen.name, output.stats.functions,
                                       output.stats.instructions, output.files.size()).c_str());
    }
    table += nx::format("\nconst PsvModuleCode* const corpus_modules[] = {{\n{}}};\nconst unsigned corpus_module_count = {};\n", list,
                        sizeof kCorpusModules / sizeof kCorpusModules[0]);
    // (each function's flags at return, bit 3 N to bit 0 V; by code address)
    table += nx::format("\nconst uint32_t corpus_exit_flags[][2] = {{\n{}}};\nconst unsigned corpus_exit_flag_count = sizeof corpus_exit_flags / "
                        "sizeof corpus_exit_flags[0];\nconst int corpus_full_exit = {};\n",
                        exits, full_exit ? 1 : 0);
    write(dir / nx::fs::path_view("corpus_modules.c"), table);
    write(dir / nx::fs::path_view("corpus_functions.tsv"), names);
    return 0;
}
