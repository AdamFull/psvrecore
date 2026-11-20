// psvrecomp — the static recompiler: finds the code of a Vita module (SELF or ELF) and translates it to C.
//
//   psvrecomp analyze <module>                  what code discovery finds: functions, table branches, diagnostics
//   psvrecomp generate <module> <dir> --name N  the module's code as C (see recomp/generator.hpp);
//                                               --instructions-per-file N sets the size of each .c file

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "psvrecore/analysis/flow.hpp"
#include "psvrecore/analysis/image.hpp"
#include "psvrecore/analysis/program.hpp"
#include "psvrecore/arm/format.hpp"
#include "psvrecore/cli.hpp"
#include "psvrecore/error.hpp"
#include "psvrecore/formats/nids.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/recomp/generator.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/text.hpp"
#include "core/foundation/containers/map.h"
#include "core/foundation/strings/format.h"

using namespace psvrecore;

namespace {

template <class... Args>
void print(std::format_string<Args...> format, Args&&... args) {
    std::fputs(nx::format(format, std::forward<Args>(args)...).c_str(), stdout);
}

// "0x81000000,0x81100000": one base per loadable segment.
Result<nx::vector<u32>> parse_bases(nx::string_view text) {
    nx::vector<u32> out;
    if (text.empty()) return out;
    for (const auto part : split(text, ',')) {
        const auto v = parse_u64(part, 0);
        if (!v || *v > 0xFFFFFFFFu) return failure(nx::format("bad base address '{}'", part));
        out.push_back(static_cast<u32>(*v));
    }
    return out;
}

Result<nx::vector<u32>> parse_entries(const nx::vector<nx::string>& texts) {
    nx::vector<u32> out;
    for (const auto& t : texts) {
        const auto v = parse_u64(t, 0);
        if (!v || *v > 0xFFFFFFFFu) return failure(nx::format("bad entry address '{}'", t));
        out.push_back(static_cast<u32>(*v));
    }
    return out;
}

const char* severity_name(analysis::Severity s) {
    switch (s) {
        case analysis::Severity::Note: return "note";
        case analysis::Severity::Warning: return "warning";
        default: return "error";
    }
}

// The imports the NID database names as functions that never return.
nx::vector<u32> noreturn_imports(const analysis::Image& image, const nids::Database& db) {
    nx::vector<u32> out;
    for (const auto& lib : image.module().imports)
        for (const auto& f : lib.functions)
            if (const nx::string* name = db.find(lib.nid, f.nid); name && analysis::is_noreturn_name(*name)) out.push_back(f.nid);
    return out;
}

Result<void> cmd_analyze(nx::string_view path, const nx::vector<u32>& bases, analysis::Options options,
                         const nids::Database& db, bool list_functions, bool list_diagnostics) {
    auto image = analysis::Image::load(nx::fs::path_view(path), bases);
    if (!image) return nx::err(image.error());
    options.noreturn_imports = noreturn_imports(*image, db);
    const auto program = analysis::analyze(*image, options);
    usize noreturn = 0;
    for (const auto& [code, fn] : program.functions) noreturn += !fn.returns;
    usize thumb = 0, instructions = 0, tables = 0, unbounded = 0, runs_into = 0, labels = 0;
    nx::map<nx::string, usize> ops;
    for (const auto& [code, fn] : program.functions) {
        thumb += fn.mode == arm::Mode::Thumb;
        instructions += fn.code.size();
        tables += fn.tables.size();
        for (const auto& [site, t] : fn.tables) unbounded += !t.bounded;
        runs_into += fn.runs_into.size();
        labels += fn.labels.size();
        if (list_functions)
            print("function 0x{:08x} {} {} instructions, {} labels, {} tables\n", code,
                  fn.mode == arm::Mode::Thumb ? "thumb" : "arm", fn.code.size(), fn.labels.size(), fn.tables.size());
    }
    print("module      {}\n", image->module().info.name);
    for (const auto& s : image->segments())
        print("segment {}   0x{:08x}-0x{:08x} {}{}{}\n", s.index, s.base, s.base + s.size(),
              s.flags & elf::kSegmentRead ? 'r' : '-', s.flags & elf::kSegmentWrite ? 'w' : '-',
              s.flags & elf::kSegmentExecute ? 'x' : '-');
    print("functions   {} ({} Thumb, {} ARM)\n", program.functions.size(), thumb, program.functions.size() - thumb);
    print("code        {} instructions (shared code counted in each function), {} labels\n", instructions, labels);
    print("tables      {} ({} with their size inferred)\n", tables, unbounded);
    print("runs into   {} places where a function continues into another\n", runs_into);
    print("no return   {} functions never return; {} imports never return\n", noreturn, options.noreturn_imports.size());
    print("imports     {} functions\n", program.imports.size());
    nx::map<nx::string, usize> kinds;
    for (const auto& d : program.diagnostics) {
        // (grouped by message without its addresses)
        nx::string key = nx::format("{}: ", severity_name(d.severity));
        bool in_hex = false;
        for (usize i = 0; i < d.message.size(); ++i) {
            const char c = d.message[i];
            if (c == '0' && i + 1 < d.message.size() && d.message[i + 1] == 'x') {
                key += "N";
                in_hex = true;
                ++i;
                continue;
            }
            if (in_hex && ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) continue;
            in_hex = false;
            if (c == ':') break;
            key += c;
        }
        ++kinds[key];
        if (list_diagnostics)
            print("{} at 0x{:08x} (function 0x{:08x}): {}\n", severity_name(d.severity), d.address, d.function, d.message);
    }
    for (const auto& [k, n] : kinds) print("  {:6} {}\n", n, k);
    return {};
}

// "nzcv" (any of the letters), "none" or "all": a set of condition flags.
Result<u8> parse_flags(nx::string_view text) {
    if (text.empty() || text == "none") return u8(0);
    if (text == "all") return u8(analysis::kAllFlags);
    u8 f = 0;
    for (const char c : text) {
        if (c == 'n' || c == 'N') f |= analysis::kFlagN;
        else if (c == 'z' || c == 'Z') f |= analysis::kFlagZ;
        else if (c == 'c' || c == 'C') f |= analysis::kFlagC;
        else if (c == 'v' || c == 'V') f |= analysis::kFlagV;
        else return failure(nx::format("bad flags '{}' (letters of nzcv, none or all)", text));
    }
    return f;
}

struct FlagArgs {
    bool everywhere = false;
    nx::string given_at_entry, given_at_return;
    bool registers_in_context = false, no_callee_saved = false;
    nx::string keep_registers = "all";
    bool no_sp_check = false;
    bool vfp_in_context = false;
    bool vfp_calls_conservative = false;
    bool neon_in_context = false;
    bool no_q_locals = false;
    bool no_internal_calls = false;
    nx::vector<nx::string> no_inline;
    bool no_call_table = false, call_table_inline = false;
    u32 inline_limit = recomp::Options{}.inline_limit;
    bool inline_in_file = false;
};

bool valid_identifier(nx::string_view name) {
    if (name.empty() || (name[0] >= '0' && name[0] <= '9')) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

Result<void> cmd_generate(nx::string_view path, nx::string_view out_dir, const nx::string& name, const nx::vector<u32>& bases,
                          analysis::Options options, const nids::Database& db, u32 instructions_per_file, const FlagArgs& flag_args) {
    if (!valid_identifier(name)) return failure(nx::format("--name '{}' is not a C identifier", name));
    if (instructions_per_file == 0) return failure("--instructions-per-file must be at least 1");
    const auto given_at_entry = parse_flags(flag_args.given_at_entry);
    if (!given_at_entry) return nx::err(given_at_entry.error());
    const auto given_at_return = parse_flags(flag_args.given_at_return);
    if (!given_at_return) return nx::err(given_at_return.error());
    auto image = analysis::Image::load(nx::fs::path_view(path), bases);
    if (!image) return nx::err(image.error());
    options.noreturn_imports = noreturn_imports(*image, db);
    const auto program = analysis::analyze(*image, options);
    usize errors = 0;
    for (const auto& d : program.diagnostics) errors += d.severity == analysis::Severity::Error;
    recomp::Options gen;
    gen.name = name;
    gen.instructions_per_file = instructions_per_file;
    gen.flags_everywhere = flag_args.everywhere;
    gen.flags_given_at_entry = *given_at_entry;
    gen.flags_given_at_return = *given_at_return;
    gen.registers_in_context = flag_args.registers_in_context;
    gen.no_callee_saved = flag_args.no_callee_saved;
    gen.check_sp = !flag_args.no_sp_check;
    gen.vfp_in_context = flag_args.vfp_in_context;
    gen.vfp_calls_conservative = flag_args.vfp_calls_conservative;
    gen.neon_in_context = flag_args.neon_in_context;
    gen.no_q_locals = flag_args.no_q_locals;
    gen.internal_calls = !flag_args.no_internal_calls;
    gen.call_table = !flag_args.no_call_table;
    gen.call_table_inline = flag_args.call_table_inline;
    gen.inline_limit = flag_args.inline_limit;
    gen.noinline_internal = !flag_args.inline_in_file;
    {
        const auto listed = parse_entries(flag_args.no_inline);
        if (!listed) return nx::err(listed.error());
        gen.no_internal = *listed;
    }
    if (flag_args.keep_registers == "all") gen.keep_registers = analysis::KeepPolicy::All;
    else if (flag_args.keep_registers == "loops") gen.keep_registers = analysis::KeepPolicy::Loops;
    else if (flag_args.keep_registers == "none") gen.keep_registers = analysis::KeepPolicy::None;
    else return failure(nx::format("--keep-registers '{}': all, loops or none", flag_args.keep_registers));
    const auto output = recomp::generate(program, gen);
    const nx::fs::path_view dir(out_dir);
    if (auto made = nx::fs::create_directories(dir); !made)
        return failure(nx::format("cannot create {}", dir.native()));
    for (const auto& f : output.files) {
        const nx::string file_path = dir / nx::fs::path_view(f.name);
        if (auto written = io::write_text_file(nx::fs::path_view(file_path), f.text); !written) return written;
    }
    // (PSVRECOMP_ENTRIES=<file>: each function's internal entry, for looking into the analysis)
    if (const char* list = std::getenv("PSVRECOMP_ENTRIES")) {
        static constexpr const char* kKinds[] = {"public", "inside", "flushed"};
        nx::string text = "function\tentry\treason\tparams\tresults\tmodified\tpreserved\treturns_to_lr\n";
        for (const auto& [code, r] : output.registers.functions)
            text += nx::format("{:08x}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", code, kKinds[static_cast<int>(r.entry)], analysis::entry_reason_name(r.reason),
                               analysis::register_names(r.params), analysis::register_names(r.results), analysis::register_names(r.modified),
                               analysis::register_names(r.preserved), r.returns_to_lr ? 1 : 0);
        if (auto written = io::write_text_file(nx::fs::path_view(list), text); !written) return written;
    }
    const auto& st = output.stats;
    print("module      {} -> {} ({} files)\n", image->module().info.name, dir.native(), output.files.size());
    print("functions   {}, {} instructions\n", st.functions, st.instructions);
    print("calls       {} to imports, {} indirect; {} tail calls, {} indirect jumps, {} jump tables\n", st.import_calls,
          st.indirect_calls, st.tail_calls, st.indirect_jumps, st.jump_tables);
    print("literals    {} loads done at generation time\n", st.literals_folded);
    const auto& fl = output.flags;
    print("flags       {} of {} flag-setting instructions compute a flag; {} of {} calls write flags to the context, "
          "{} read them back; {} of {} returns leave flags\n",
          st.flag_setters_live, st.flag_setters, st.call_sites_store, st.call_sites, st.call_sites_load, st.return_sites_store,
          st.return_sites);
    print("            {} functions read a flag at entry, {} leave flags for their callers{}\n", st.functions_need,
          st.functions_live_out, fl.everywhere ? " (--flags-everywhere)" : "");
    print("            contract: read at entry {}, given at entry {}, read after indirect calls {}, given at return {}\n",
          analysis::flag_names(fl.needed_at_entry), analysis::flag_names(fl.given_at_entry), analysis::flag_names(fl.needed_at_return),
          analysis::flag_names(fl.given_at_return));
    if (gen.registers_in_context) {
        print("registers   in the context (--registers-in-context)\n");
    } else {
        print("registers   {} read at entry, {} written to the context, {} read back after calls ({} at traps)\n",
              st.register_entry_loads, st.register_stores, st.register_call_loads, st.register_trap_stores);
        nx::string kept;
        for (u32 k = 4; k < 14; ++k) kept += nx::format(" {}:{}", k == 13 ? nx::string("sp") : nx::format("r{}", k), st.functions_preserving[k]);
        print("            {} direct calls keep {} registers in locals; {} calls check the stack pointer{}\n", st.calls_keeping,
              st.registers_kept, st.stack_checks, gen.no_callee_saved ? " (--no-callee-saved)" : "");
        print("            functions preserving r4-r11 and sp: {} all;{} ({} check sp)\n", st.functions_preserving_all, kept,
              st.functions_checking_sp);
        const auto& uw = output.registers.unwind;
        if (uw.entries || uw.undecoded) {
            print("            exception index: {} functions with unwind opcodes: {} agree, {} preserve a register they write that "
                  "unwinding does not restore{}; {} restore by unwinding one not shown preserved; {} entries without a frame, not "
                  "unwinding or not decoded\n",
                  uw.entries, uw.agree, uw.disagree, uw.disagreeing.empty() ? nx::string() : nx::format(" (0x{:08x}...)", uw.disagreeing[0]),
                  uw.unproven, uw.undecoded);
        }
        if (gen.internal_calls) {
            const auto reasons = [](const nx::map<nx::string, usize>& m) {
                nx::string s;
                for (const auto& [why, n] : m) s += nx::format("{}{} {}", s.empty() ? "" : ", ", n, why);
                return s.empty() ? nx::string("none") : s;
            };
            print("calls       internal entries: {} reach nothing outside, {} may ({}); {} functions without one ({})\n",
                  st.internal_inside, st.internal_flushed, reasons(st.flushed_reasons),
                  st.functions - st.internal_inside - st.internal_flushed, reasons(st.public_reasons));
            print("            direct calls: {} to internal entries without the context, {} with it ({}), {} through f_<address> ({}); "
                  "{} returns without a compare; {} arguments, {} results\n",
                  st.calls_inside, st.calls_flushed, reasons(st.calls_flushed_reasons), st.calls_public, reasons(st.calls_public_reasons),
                  st.internal_returns, st.internal_params, st.internal_results);
            print("            inlined: {} functions (at most {} instructions), {} direct calls of them, {} static copies in the .c files\n",
                  st.functions_inlined, gen.inline_limit, st.calls_inlined, st.inline_copies);
        } else {
            print("calls       every direct call through f_<address> (--no-internal-calls)\n");
        }
        if (st.register_effect_mismatches) {
            print("            {} instructions' translations read or write other registers than the analysis' model says:\n",
                  st.register_effect_mismatches);
            for (const auto& note : st.register_notes) print("              {}\n", note);
        }
    }
    if (gen.vfp_in_context) {
        print("vfp         in the context (--vfp-in-context)\n");
    } else {
        print("vfp         {} functions with floating-point locals ({} D registers as two floats, {} as a double); {} read at "
              "entry\n",
              st.vfp_functions, st.vfp_s_held, st.vfp_d_held, st.vfp_entry_loads);
        print("            {} places write locals to the context, {} read them back ({} Advanced SIMD instructions); {} calls "
              "keep a live local the callee does not write{}; {} at traps\n",
              st.vfp_stores, st.vfp_call_loads, st.vfp_simd_syncs, st.vfp_calls_keeping,
              gen.vfp_calls_conservative ? " (--vfp-calls-conservative)" : "", st.vfp_trap_stores);
        print("            {} vcmp + vmrs pairs compare once, {} conditions compare their operands\n", st.vfp_fused_compares,
              st.vfp_fused_conditions);
        print("neon        {} Advanced SIMD instructions on values, {} in the context form{}; {} Q registers held as one vector{}\n",
              st.neon_values, st.neon_context, gen.neon_in_context ? " (--neon-in-context)" : "", st.vfp_q_held,
              gen.no_q_locals ? " (--no-q-locals)" : "");
    }
    print("discovery   {} errors (psvrecomp analyze --diagnostics lists them)\n", errors);
    usize traps = 0;
    for (const auto& [what, n] : st.traps) traps += n;
    print("traps       {} instructions translated to a trap\n", traps);
    for (const auto& [what, n] : st.traps) print("  {:6} {}\n", n, what);
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    nx::string file, bases_text, out_dir, name;
    nx::vector<nx::string> entries_text, dbs;
    bool no_default_db = false;
    bool list_functions = false, list_diagnostics = false, no_exidx = false;
    u32 instructions_per_file = recomp::Options{}.instructions_per_file;
    FlagArgs flag_args;
    nx::cli::Parser cli("psvrecomp", "Finds the code of a Vita module and translates it to C.");
    cli.command("analyze", "code discovery: functions, table branches, diagnostics")
        .positional("file", file, "SELF or ELF module")
        .option("base", bases_text, "guest address of each loadable segment, comma-separated", {.value_name = "ADDRESSES"})
        .option_many("entry", entries_text, "an extra function entry (code address, bit 0 for Thumb); repeatable", {.value_name = "ADDRESS"})
        .flag("no-exidx", no_exidx, "do not take function starts from the exception index table")
        .flag("functions", list_functions, "list the functions")
        .flag("diagnostics", list_diagnostics, "list every diagnostic")
        .option_many("db", dbs, "NID database (.yml, .tsv, or a directory of them); repeatable", {.value_name = "PATH"})
        .flag("no-default-db", no_default_db, "do not load vita-headers' database");
    cli.command("generate", "translate the module's code to C")
        .positional("file", file, "SELF or ELF module")
        .positional("out", out_dir, "the folder to write the C files to")
        .option("name", name, "C identifier prefix of the module's tables", {.value_name = "NAME"})
        .option("base", bases_text, "guest address of each loadable segment, comma-separated", {.value_name = "ADDRESSES"})
        .option_many("entry", entries_text, "an extra function entry (code address, bit 0 for Thumb); repeatable", {.value_name = "ADDRESS"})
        .flag("no-exidx", no_exidx, "do not take function starts from the exception index table")
        .option("instructions-per-file", instructions_per_file,
                "start a new .c file once a file holds this many guest instructions (default 20000)", {.value_name = "N"})
        .flag("flags-everywhere", flag_args.everywhere,
              "no condition-flag liveness: every flag computed, and in the context at every call, jump and return")
        .option("flags-given-at-entry", flag_args.given_at_entry,
                "condition flags other modules' functions read at entry (put in the context before every call or jump "
                "to code outside this module): letters of nzcv, none or all", {.value_name = "FLAGS"})
        .option("flags-given-at-return", flag_args.given_at_return,
                "condition flags other modules read after an indirect call returns (left in the context by every "
                "function's return)", {.value_name = "FLAGS"})
        .flag("registers-in-context", flag_args.registers_in_context,
              "core registers in the context everywhere, not in locals (for checking a suspected register analysis bug)")
        .flag("no-callee-saved", flag_args.no_callee_saved,
              "no callee-saved knowledge: every register live after a call is read back from the context")
        .option("keep-registers", flag_args.keep_registers,
                "where a caller keeps the registers a callee preserves in locals instead of reading them back: all "
                "(direct calls), loops (direct calls inside loops) or none", {.value_name = "WHERE"})
        .flag("vfp-in-context", flag_args.vfp_in_context,
              "floating-point registers and the FPSCR in the context everywhere, not in locals (the translation from before "
              "they were locals, for checking a suspected floating-point locals bug)")
        .flag("neon-in-context", flag_args.neon_in_context,
              "Advanced SIMD instructions in the context form, guest_simd.h's helpers with the floating-point locals synced "
              "around each (the translation from before they were on values, for checking a suspected NEON bug)")
        .flag("no-q-locals", flag_args.no_q_locals, "no Q register held as one vector local (each as two D locals)")
        .flag("vfp-calls-conservative", flag_args.vfp_calls_conservative,
              "every call taken to read and write all floating-point registers: none kept in locals across a call")
        .flag("no-internal-calls", flag_args.no_internal_calls,
              "no internal entries: every direct call through f_<address>, every register in the context before it (the "
              "translation from before them, for checking a suspected bug in them)")
        .option_many("no-inline", flag_args.no_inline,
                     "a function (code address) given no internal entry and never inlined: every direct call to it goes "
                     "through f_<address>, which a game may stand in for; repeatable", {.value_name = "ADDRESS"})
        .option("inline-limit", flag_args.inline_limit,
                "inline the internal entries of at most N instructions without a jump table into their callers (a static "
                "copy in each .c file that calls them); 0: none (default 8)", {.value_name = "N"})
        .flag("inline-in-file", flag_args.inline_in_file,
              "let the C compiler inline the internal entries that are not inlined (by --inline-limit) into the callers in "
              "their own .c file (by default they are noinline: a copy there would keep the out-of-line one too)")
        .flag("call-table-inline", flag_args.call_table_inline,
              "each indirect call site looks in the module's table itself (a larger program) instead of calling the "
              "module's function that does")
        .flag("no-call-table", flag_args.no_call_table,
              "indirect calls through the runtime's lookup (psv_call) only, not first through the module's table")
        .flag("no-sp-check", flag_args.no_sp_check,
              "take calls to code outside the module to return with the stack pointer they were called with, without "
              "checking it after them")
        .option_many("db", dbs, "NID database (.yml, .tsv, or a directory of them); repeatable", {.value_name = "PATH"})
        .flag("no-default-db", no_default_db, "do not load vita-headers' database");
    nx::string command;
    if (const int code = parse_command_line(cli, argc, argv, &command); code >= 0) return code;
    if (command.empty()) {
        std::fputs(cli.help().c_str(), stderr);
        return 2;
    }
    const auto run = [&]() -> Result<void> {
        const auto bases = parse_bases(bases_text);
        if (!bases) return nx::err(bases.error());
        const auto entries = parse_entries(entries_text);
        if (!entries) return nx::err(entries.error());
        analysis::Options options;
        options.extra_entries = *entries;
        options.use_exidx = !no_exidx;
        nids::Database db;
        if (!no_default_db)
            if (auto loaded = db.load(PSVRECORE_NID_DB_DIR); !loaded)
                return nx::err(Error{nx::format("default NID database: {} (give --no-default-db to skip it)", loaded.error().message)});
        for (const auto& path : dbs)
            if (auto loaded = db.load(path); !loaded) return loaded;
        if (command == "analyze") return cmd_analyze(file, *bases, options, db, list_functions, list_diagnostics);
        if (command == "generate") return cmd_generate(file, out_dir, name, *bases, options, db, instructions_per_file, flag_args);
        return failure(nx::format("unknown command '{}'", command));
    };
    if (const auto done = run(); !done) {
        nx::loge(logs::tool, "psvrecomp: {}", done.error().message);
        return 1;
    }
    return 0;
}
