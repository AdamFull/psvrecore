// psvinfo — inspects Vita executables and modules (SELF or ELF), SFO files, CPK archives and NID databases: module
// information, imports and exports by name, relocations, the ELF inside a SELF, and the files a CPK holds.

#include <algorithm>
#include <cstdio>
#include <optional>

#include "psvrecore/cli.hpp"
#include "psvrecore/error.hpp"
#include "psvrecore/formats/cpk.hpp"
#include "psvrecore/formats/elf.hpp"
#include "psvrecore/formats/module.hpp"
#include "psvrecore/formats/nids.hpp"
#include "psvrecore/formats/reloc.hpp"
#include "psvrecore/formats/self.hpp"
#include "psvrecore/formats/sfo.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/log.hpp"
#include "core/foundation/containers/map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

using namespace psvrecore;

namespace {

// The tool's output (stdout).
template <class... Args>
void print(std::format_string<Args...> format, Args&&... args) {
    std::fputs(nx::format(format, std::forward<Args>(args)...).c_str(), stdout);
}

// A file as read, and the ELF it is or holds.
struct Input {
    nx::blob<u8> data;
    std::optional<self::File> self;
    nx::vector<u8> elf_bytes;  // the ELF extracted from a SELF (empty when the file is an ELF)
    std::span<const u8> elf() const { return self ? std::span<const u8>(elf_bytes) : io::view(data); }
};

Result<void> load_input(nx::fs::path_view path, Input& in) {
    auto data = io::read_file(path);
    if (!data) return nx::err(data.error());
    in.data = std::move(*data);
    if (self::is_self(io::view(in.data))) {
        auto parsed = self::parse(io::view(in.data));
        if (!parsed) return nx::err(parsed.error());
        in.self = std::move(*parsed);
        auto elf = self::to_elf(io::view(in.data));
        if (!elf) return nx::err(elf.error());
        in.elf_bytes = std::move(*elf);
    }
    return {};
}

struct Names {
    nids::Database db;
    const nx::string* function(u32 library, u32 nid) const { return db.find(library, nid); }
};

Result<void> load_names(Names& names, const nx::vector<nx::string>& paths, bool default_db) {
    if (default_db) {
        if (auto loaded = names.db.load(PSVRECORE_NID_DB_DIR); !loaded)
            return nx::err(Error{nx::format("default NID database: {} (give --no-default-db to skip it)", loaded.error().message)});
    }
    for (const auto& p : paths)
        if (auto loaded = names.db.load(p); !loaded) return loaded;
    return {};
}

nx::string flags_text(u32 flags) {
    nx::string s;
    s.push_back(flags & elf::kSegmentRead ? 'r' : '-');
    s.push_back(flags & elf::kSegmentWrite ? 'w' : '-');
    s.push_back(flags & elf::kSegmentExecute ? 'x' : '-');
    return s;
}

const char* segment_type_name(u32 type) {
    switch (type) {
        case elf::kSegmentLoad: return "LOAD";
        case elf::kSegmentSceRela: return "SCE_RELA";
        case elf::kSegmentSceComment: return "SCE_COMMENT";
        case elf::kSegmentSceVersion: return "SCE_VERSION";
        case elf::kSegmentArmExidx: return "ARM_EXIDX";
        default: return "?";
    }
}

nx::string hex_or_dash(u32 v) { return v ? nx::format("0x{:08x}", v) : nx::string("-"); }

// How many of a library's symbols the database names.
usize named_count(const Names& names, const module::Library& lib) {
    usize n = 0;
    for (const auto& s : lib.functions) n += names.function(lib.nid, s.nid) != nullptr;
    for (const auto& s : lib.variables) n += names.function(lib.nid, s.nid) != nullptr;
    for (const auto& s : lib.tls_variables) n += names.function(lib.nid, s.nid) != nullptr;
    return n;
}
usize symbol_count(const module::Library& lib) { return lib.functions.size() + lib.variables.size() + lib.tls_variables.size(); }

// The relocations, and whether each one already holds its value for the addresses the module was linked at.
struct RelocCheck {
    nx::vector<reloc::Entry> entries;
    usize applied = 0;
    nx::vector<std::pair<reloc::Entry, nx::string>> problems;  // not applied, or not evaluable
};
Result<RelocCheck> check_relocations(const elf::File& file) {
    auto entries = reloc::decode_all(file);
    if (!entries) return nx::err(entries.error());
    RelocCheck out;
    out.entries = std::move(*entries);
    // A copy of each segment's file data, at its link address (the check only reads it).
    nx::vector<nx::vector<u8>> copies;
    nx::vector<reloc::Segment> segments;
    for (const auto& s : file.segments()) {
        const auto bytes = file.contents(s);
        copies.emplace_back(bytes.begin(), bytes.end());
    }
    for (usize i = 0; i < copies.size(); ++i)
        segments.push_back(reloc::Segment{file.segments()[i].vaddr, std::span<u8>(copies[i])});
    for (const auto& e : out.entries) {
        const auto applied = reloc::is_applied(e, segments);
        if (!applied)
            out.problems.emplace_back(e, applied.error().message);
        else if (*applied)
            ++out.applied;
        else
            out.problems.emplace_back(e, "the linked contents differ from the relocation's value");
    }
    return out;
}

Result<void> print_sfo(std::span<const u8> data) {
    const auto sfo = sfo::parse(data);
    if (!sfo) return nx::err(sfo.error());
    print("container SFO (version 0x{:04x}), {} entries\n", sfo->version, sfo->entries.size());
    for (const auto& e : sfo->entries) {
        if (e.format == sfo::Format::Int32)
            print("  {:<24} 0x{:08x} ({})\n", e.key, e.integer, e.integer);
        else
            print("  {:<24} \"{}\"\n", e.key, e.text);
    }
    return {};
}

Result<void> cmd_info(nx::fs::path_view path, const Names& names) {
    Input in;
    {
        auto data = io::read_file(path);
        if (!data) return nx::err(data.error());
        if (sfo::is_sfo(io::view(*data))) return print_sfo(io::view(*data));
    }
    if (auto loaded = load_input(path, in); !loaded) return loaded;
    print("file      {}\n", path.native());
    if (in.self) {
        const auto& h = in.self->header;
        print("container SELF version {}, SDK type 0x{:x}, {}\n", h.version, h.sdk_type,
              in.self->encrypted() ? "encrypted" : "plaintext");
        const auto& a = in.self->app_info;
        print("app info  auth ID 0x{:016x}, vendor 0x{:08x}, SELF type {}, version 0x{:016x}\n", a.auth_id, a.vendor_id,
              a.self_type, a.version);
    } else {
        print("container ELF\n");
    }
    const auto file = elf::File::parse(in.elf());
    if (!file) return nx::err(file.error());
    print("elf       type 0x{:04x}, machine {}, entry 0x{:08x}, flags 0x{:08x}\n", file->type(), file->machine(),
          file->entry(), file->flags());
    for (usize i = 0; i < file->segments().size(); ++i) {
        const auto& s = file->segments()[i];
        nx::string where;
        if (in.self && i < in.self->segments.size()) {
            const auto& si = in.self->segments[i];
            where = nx::format("  [SELF 0x{:x}, {}{}]", si.offset, si.compressed() ? "zlib" : "stored",
                               si.encrypted() ? ", encrypted" : "");
        }
        print("segment {} {:<11} vaddr 0x{:08x}  filesz 0x{:08x}  memsz 0x{:08x}  {}  align 0x{:x}{}\n", i,
              segment_type_name(s.type), s.vaddr, s.filesz, s.memsz, flags_text(s.flags), s.align, where);
    }

    const auto m = module::parse(*file);
    if (!m) return nx::err(m.error());
    const auto& info = m->info;
    print("module    {}  version {}.{}, type {}, attributes 0x{:04x}, NID 0x{:08x}, info at 0x{:08x}\n", info.name,
          info.version >> 8, info.version & 0xFF, info.type, info.attributes, info.module_nid, info.address);
    print("          start {}  stop {}  gp {}\n", hex_or_dash(info.module_start), hex_or_dash(info.module_stop),
          hex_or_dash(info.gp));
    print("          TLS {} (file 0x{:x}, memory 0x{:x})  exidx {}-{}  extab {}-{}\n", hex_or_dash(info.tls_start),
          info.tls_file_size, info.tls_memory_size, hex_or_dash(info.exidx_start), hex_or_dash(info.exidx_end),
          hex_or_dash(info.extab_start), hex_or_dash(info.extab_end));

    for (const auto& lib : m->exports) {
        if (lib.attributes & module::kExportMain) {
            nx::string list;
            for (const auto* group : {&lib.functions, &lib.variables})
                for (const auto& s : *group) {
                    const char* n = module::main_export_name(s.nid);
                    list += nx::format(" {}=0x{:08x}", n ? n : nx::format("{:08X}", s.nid).c_str(), s.address);
                }
            print("export    (module){}\n", list);
            continue;
        }
        print("export    {:<24} NID 0x{:08x}  {} functions, {} variables, {} TLS  (named {}/{})\n", lib.name, lib.nid,
              lib.functions.size(), lib.variables.size(), lib.tls_variables.size(), named_count(names, lib),
              symbol_count(lib));
    }
    usize total = 0, named = 0;
    for (const auto& lib : m->imports) {
        print("import    {:<24} NID 0x{:08x}  version {}  {} functions, {} variables, {} TLS  (named {}/{})\n", lib.name,
              lib.nid, lib.version, lib.functions.size(), lib.variables.size(), lib.tls_variables.size(),
              named_count(names, lib), symbol_count(lib));
        total += symbol_count(lib);
        named += named_count(names, lib);
    }
    print("imports   {} libraries, {} symbols, {} named, {} unnamed\n", m->imports.size(), total, named, total - named);

    const auto relocs = check_relocations(*file);
    if (!relocs) return nx::err(relocs.error());
    nx::map<nx::string, usize> by_kind;
    for (const auto& e : relocs->entries)
        ++by_kind[nx::format("{} seg{} -> seg{}", reloc::type_name(e.type), e.patch_segment, e.symbol_segment)];
    print("relocs    {} (patched segment -> segment the value is relative to)\n", relocs->entries.size());
    for (const auto& [k, n] : by_kind) print("          {:<34} {}\n", k, n);
    print("          {} agree with the linked contents, {} do not\n", relocs->applied, relocs->problems.size());
    for (usize i = 0; i < relocs->problems.size() && i < 20; ++i) {
        const auto& [e, why] = relocs->problems[i];
        print("          ! {} at seg{} + 0x{:x} (seg{} + 0x{:x}): {}\n", reloc::type_name(e.type), e.patch_segment, e.offset,
              e.symbol_segment, e.addend, why);
    }
    return {};
}

// Imported or exported symbols as a table.
Result<void> cmd_symbols(nx::fs::path_view path, const Names& names, bool exports) {
    Input in;
    if (auto loaded = load_input(path, in); !loaded) return loaded;
    const auto file = elf::File::parse(in.elf());
    if (!file) return nx::err(file.error());
    const auto m = module::parse(*file);
    if (!m) return nx::err(m.error());
    print("library\tlibrary_nid\tkind\tnid\taddress\tname\n");
    for (const auto& lib : exports ? m->exports : m->imports) {
        const auto row = [&](const char* kind, const module::Symbol& s) {
            const nx::string* n = names.function(lib.nid, s.nid);
            const char* special = (lib.attributes & module::kExportMain) && exports ? module::main_export_name(s.nid) : nullptr;
            print("{}\t{:08X}\t{}\t{:08X}\t0x{:08x}\t{}\n", lib.name.empty() ? nx::string("(module)") : lib.name, lib.nid,
                  kind, s.nid, s.address, n ? *n : nx::string(special ? special : "?"));
        };
        for (const auto& s : lib.functions) row("function", s);
        for (const auto& s : lib.variables) row("variable", s);
        for (const auto& s : lib.tls_variables) row("tls", s);
    }
    return {};
}

Result<void> cmd_relocs(nx::fs::path_view path) {
    Input in;
    if (auto loaded = load_input(path, in); !loaded) return loaded;
    const auto file = elf::File::parse(in.elf());
    if (!file) return nx::err(file.error());
    const auto relocs = check_relocations(*file);
    if (!relocs) return nx::err(relocs.error());
    print("type\tpatch_segment\toffset\tsymbol_segment\taddend\n");
    for (const auto& e : relocs->entries)
        print("{}\t{}\t0x{:x}\t{}\t0x{:x}\n", reloc::type_name(e.type), e.patch_segment, e.offset, e.symbol_segment, e.addend);
    return {};
}

Result<void> cmd_elf(nx::fs::path_view path, nx::fs::path_view out) {
    Input in;
    if (auto loaded = load_input(path, in); !loaded) return loaded;
    if (!in.self) return failure(nx::format("'{}' is not a SELF", path.native()));
    if (auto written = io::write_file(out, in.elf_bytes); !written) return written;
    print("{} bytes written to {}\n", in.elf_bytes.size(), out.native());
    return {};
}

Result<void> cmd_stubs(nx::fs::path_view dir, nx::fs::path_view out) {
    const auto entries = nx::fs::directory_entries(dir);
    if (!entries) return failure(nx::format("cannot list directory '{}'", dir.native()));
    nx::vector<nx::string> archives;
    for (const auto& e : entries.value())
        if (e.type == nx::fs::file_type::regular && nx::fs::path_view(e.path).extension() == ".a") archives.push_back(e.path);
    std::sort(archives.begin(), archives.end());
    nids::Database db;
    for (const auto& a : archives) {
        const auto data = io::read_file(a);
        if (!data) return nx::err(data.error());
        const auto found = nids::stub_archive_nids(io::view(*data));
        if (!found) continue;  // not an archive this understands
        for (const auto& l : found->libraries) db.add_library(l.nid, l.name);
        for (const auto& n : found->names) db.add(n.library_nid, n.nid, n.kind, n.name);
    }
    if (auto written = io::write_text_file(out, db.to_tsv()); !written) return written;
    print("{} archives, {} libraries, {} names written to {}\n", archives.size(), db.library_count(), db.size(), out.native());
    return {};
}

nx::string extension_of(nx::string_view name) {
    const usize dot = name.rfind('.');
    return dot == nx::string_view::npos ? nx::string("(none)") : nx::string(name.substr(dot + 1));
}

// A CPK's header and files: totals, by extension, and (with `list`) every file, tab-separated. Each file's first
// bytes are read to check its offset: a compressed file starts with "CRILAYLA".
Result<void> cmd_cpk(nx::fs::path_view path, bool list) {
    auto opened = nx::fs::open_file(path);
    if (!opened) return failure(nx::format("cannot open '{}': {}", path.native(), nx::fs::to_string(opened.error().kind)));
    nx::fs::stream& s = **opened;
    const i64 size = s.size();
    if (size < 0) return failure(nx::format("'{}': size unknown", path.native()));
    const auto read = [&](u64 offset, void* out, usize bytes) {
        return s.seek(static_cast<i64>(offset)) && s.read(out, bytes) == bytes;
    };
    const auto archive = cpk::read_archive(static_cast<u64>(size), read);
    if (!archive) return nx::err(archive.error());
    const auto& a = *archive;

    struct Totals {
        usize files = 0, compressed = 0;
        u64 stored = 0, extracted = 0;
    };
    Totals all;
    nx::map<nx::string, Totals> by_extension;
    nx::map<nx::string, usize> by_top_folder;
    usize marker_mismatch = 0;
    nx::vector<nx::string> rows;
    for (const auto& f : a.files) {
        for (Totals* t : {&all, &by_extension[extension_of(f.name)]}) {
            ++t->files;
            t->compressed += f.compressed();
            t->stored += f.size;
            t->extracted += f.extract_size;
        }
        const usize slash = f.folder.view().find('/');
        ++by_top_folder[f.folder.empty() ? nx::string("(top)") : nx::string(f.folder.view().substr(0, slash))];
        char magic[8] = {};
        const bool marked = f.size >= 8 && read(f.offset, magic, 8) && nx::string_view(magic, 8) == "CRILAYLA";
        marker_mismatch += marked != f.compressed();
        if (list)
            rows.push_back(nx::format("{}\t{}\t0x{:x}\t{}\t{}\t{}\n", f.path(), f.id ? nx::format("{}", *f.id) : nx::string("-"),
                                      f.offset, f.size, f.extract_size, f.compressed() ? "CRILAYLA" : "stored"));
    }
    if (list) {
        print("path\tid\toffset\tsize\textract_size\tstorage\n");
        for (const auto& r : rows) print("{}", r);
        return {};
    }
    print("file      {}\n", path.native());
    print("container CPK, {} bytes, tool \"{}\", alignment {}\n", size, a.tool_version, a.align);
    print("chunks    content 0x{:x}  TOC 0x{:x}  ITOC {}  ETOC {}  GTOC {}\n", a.content_offset, a.toc_offset,
          hex_or_dash(static_cast<u32>(a.itoc_offset)), hex_or_dash(static_cast<u32>(a.etoc_offset)),
          hex_or_dash(static_cast<u32>(a.gtoc_offset)));
    print("header   ");
    for (usize c = 0; c < a.header.columns.size(); ++c) {
        const auto& col = a.header.columns[c];
        if (col.storage == cpk::Storage::Zero) continue;
        if (const auto v = a.header.integer(0, col.name)) print(" {}={}", col.name, *v);
        else if (const auto t = a.header.text(0, col.name)) print(" {}=\"{}\"", col.name, *t);
    }
    print("\n");
    print("files     {} listed ({} declared), {} compressed; {} bytes stored, {} extracted\n", all.files, a.declared_files,
          all.compressed, all.stored, all.extracted);
    print("check     {} files whose first bytes disagree with their stored size (CRILAYLA or not)\n", marker_mismatch);
    print("by extension (files, compressed, stored, extracted)\n");
    for (const auto& [ext, t] : by_extension)
        print("  {:<12} {:>7} {:>7} {:>13} {:>13}\n", ext, t.files, t.compressed, t.stored, t.extracted);
    print("by top folder (files)\n");
    for (const auto& [folder, n] : by_top_folder) print("  {:<24} {}\n", folder, n);
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    nx::string file, out;
    nx::vector<nx::string> dbs;
    bool no_default_db = false;
    bool list = false;
    nx::cli::Parser cli("psvinfo",
                        "Inspects Vita executables and modules (SELF or ELF), SFO files, CPK archives and NID databases.");
    const auto with_db = [&](nx::cli::Command& c) -> nx::cli::Command& {
        return c.option_many("db", dbs, "NID database (.yml, .tsv, or a directory of them); repeatable", {.value_name = "PATH"})
            .flag("no-default-db", no_default_db, "do not load vita-headers' database");
    };
    with_db(cli.command("info", "container, segments, module information, imports/exports, relocations; or an SFO")
                .positional("file", file, "SELF, ELF or SFO file"));
    with_db(cli.command("imports", "imported functions and variables, with names (tab-separated)").positional("file", file, "SELF or ELF"));
    with_db(cli.command("exports", "exported functions and variables, with names (tab-separated)").positional("file", file, "SELF or ELF"));
    cli.command("cpk", "a CPK archive's header and files: totals and by extension, or every file (tab-separated)")
        .positional("file", file, "CPK archive")
        .flag("list", list, "every file: path, ID, offset, stored and extracted size (tab-separated)");
    cli.command("relocs", "every relocation (tab-separated)").positional("file", file, "SELF or ELF");
    cli.command("elf", "write the ELF a SELF holds")
        .positional("file", file, "SELF")
        .positional("out", out, "the ELF file to write");
    cli.command("stubs", "build a NID database (.tsv) from SDK stub libraries (lib*_stub.a)")
        .positional("dir", file, "folder with the stub libraries")
        .positional("out", out, "the database to write");
    nx::string command;
    if (const int code = parse_command_line(cli, argc, argv, &command); code >= 0) return code;
    if (command.empty()) {
        std::fputs(cli.help().c_str(), stderr);
        return 2;
    }
    const auto run = [&]() -> Result<void> {
        if (command == "relocs") return cmd_relocs(file);
        if (command == "cpk") return cmd_cpk(file, list);
        if (command == "elf") return cmd_elf(file, out);
        if (command == "stubs") return cmd_stubs(file, out);
        Names names;
        if (auto loaded = load_names(names, dbs, !no_default_db); !loaded) return loaded;
        if (command == "info") return cmd_info(file, names);
        return cmd_symbols(file, names, command == "exports");
    };
    if (const auto done = run(); !done) {
        nx::loge(logs::tool, "psvinfo: {}", done.error().message);
        return 1;
    }
    return 0;
}
