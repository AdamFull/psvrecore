// psvrecomp — the static recompiler: finds the code of a Vita module (SELF or ELF) and translates it to C.
//
//   psvrecomp analyze <module>                  what code discovery finds: functions, table branches, diagnostics
//   psvrecomp generate <module> <dir> --name N  the module's code as C (see recomp/generator.hpp)

#include <algorithm>
#include <cstdio>

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

bool valid_identifier(nx::string_view name) {
    if (name.empty() || (name[0] >= '0' && name[0] <= '9')) return false;
    for (const char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) return false;
    return true;
}

Result<void> cmd_generate(nx::string_view path, nx::string_view out_dir, const nx::string& name, const nx::vector<u32>& bases,
                          analysis::Options options, const nids::Database& db) {
    if (!valid_identifier(name)) return failure(nx::format("--name '{}' is not a C identifier", name));
    auto image = analysis::Image::load(nx::fs::path_view(path), bases);
    if (!image) return nx::err(image.error());
    options.noreturn_imports = noreturn_imports(*image, db);
    const auto program = analysis::analyze(*image, options);
    usize errors = 0;
    for (const auto& d : program.diagnostics) errors += d.severity == analysis::Severity::Error;
    recomp::Options gen;
    gen.name = name;
    const auto output = recomp::generate(program, gen);
    const nx::fs::path_view dir(out_dir);
    if (auto made = nx::fs::create_directories(dir); !made)
        return failure(nx::format("cannot create {}", dir.native()));
    for (const auto& f : output.files) {
        const nx::string file_path = dir / nx::fs::path_view(f.name);
        if (auto written = io::write_text_file(nx::fs::path_view(file_path), f.text); !written) return written;
    }
    const auto& st = output.stats;
    print("module      {} -> {} ({} files)\n", image->module().info.name, dir.native(), output.files.size());
    print("functions   {}, {} instructions\n", st.functions, st.instructions);
    print("calls       {} to imports, {} indirect; {} tail calls, {} indirect jumps, {} jump tables\n", st.import_calls,
          st.indirect_calls, st.tail_calls, st.indirect_jumps, st.jump_tables);
    print("literals    {} loads done at generation time\n", st.literals_folded);
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
        if (command == "generate") return cmd_generate(file, out_dir, name, *bases, options, db);
        return failure(nx::format("unknown command '{}'", command));
    };
    if (const auto done = run(); !done) {
        nx::loge(logs::tool, "psvrecomp: {}", done.error().message);
        return 1;
    }
    return 0;
}
