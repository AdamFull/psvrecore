#include "psvrecore/formats/nids.hpp"

#include <algorithm>

#include "psvrecore/formats/elf.hpp"
#include "psvrecore/io/file.hpp"
#include "psvrecore/text.hpp"
#include "core/foundation/containers/map.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::nids {

namespace {

u64 key(u32 library_nid, u32 nid) { return (u64{library_nid} << 32) | nid; }

nx::string_view trim(nx::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

nx::string_view unquote(nx::string_view s) {
    if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) return s.substr(1, s.size() - 2);
    return s;
}

Result<u32> parse_nid(nx::string_view text) {
    const auto v = parse_u64(text, text.starts_with("0x") || text.starts_with("0X") ? 0 : 16);
    if (!v || *v > 0xFFFFFFFFu) return failure(nx::format("not a NID: '{}'", text));
    return static_cast<u32>(*v);
}

// "<length>_<text of that length>" at the start of `s`: the text, with `s` advanced past it.
bool take_counted(nx::string_view& s, nx::string_view& out) {
    usize n = 0, i = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') n = n * 10 + static_cast<usize>(s[i++] - '0');
    if (i == 0 || i >= s.size() || s[i] != '_' || s.size() - i - 1 < n) return false;
    out = s.substr(i + 1, n);
    s.remove_prefix(i + 1 + n);
    return true;
}

}  // namespace

void Database::add_library(u32 library_nid, nx::string_view name) { libraries_.insert_or_assign(library_nid, nx::string(name)); }

void Database::add(u32 library_nid, u32 nid, Kind kind, nx::string_view name) {
    names_.insert_or_assign(key(library_nid, nid), Name{library_nid, nid, kind, nx::string(name)});
}

const nx::string* Database::library(u32 library_nid) const {
    const auto it = libraries_.find(library_nid);
    return it == libraries_.end() ? nullptr : &it->second;
}

const nx::string* Database::find(u32 library_nid, u32 nid) const {
    const auto it = names_.find(key(library_nid, nid));
    return it == names_.end() ? nullptr : &it->second.name;
}

Result<void> Database::load_yaml(nx::string_view text) {
    // The subset vita-headers writes: two-space indented "key: value" maps, no flow syntax, no anchors.
    struct Level {
        usize indent;
        nx::string key;
    };
    nx::vector<Level> path;
    nx::map<std::pair<nx::string, nx::string>, u32> library_nids;  // (module, library) -> NID
    struct Pending {
        nx::string module, library;
        Kind kind;
        nx::string name;
        u32 nid;
    };
    nx::vector<Pending> pending;
    usize line_number = 0;
    Error error;
    bool ok = true;
    for_each_line(text, [&](nx::string_view line) {
        ++line_number;
        if (!ok) return;
        usize indent = 0;
        while (indent < line.size() && line[indent] == ' ') ++indent;
        const nx::string_view content = trim(line.substr(indent));
        if (content.empty() || content.front() == '#') return;
        const usize colon = content.find(':');
        if (colon == nx::string_view::npos) {
            ok = false;
            error = Error{nx::format("line {}: expected 'key: value'", line_number)};
            return;
        }
        const nx::string_view k = unquote(trim(content.substr(0, colon)));
        const nx::string_view v = unquote(trim(content.substr(colon + 1)));
        while (!path.empty() && path.back().indent >= indent) path.pop_back();

        if (v.empty()) {
            path.push_back(Level{indent, nx::string(k)});
            return;
        }
        if (path.empty() && k == "version" && v != "2") {
            ok = false;
            error = Error{nx::format("NID database version {} is not supported (expected 2)", v)};
            return;
        }
        const bool in_library = path.size() >= 4 && path[0].key == "modules" && path[2].key == "libraries";
        if (in_library && path.size() == 4 && k == "nid") {
            const auto nid = parse_nid(v);
            if (!nid) {
                ok = false;
                error = Error{nx::format("line {}: {}", line_number, nid.error().message)};
                return;
            }
            library_nids.insert_or_assign(std::pair(path[1].key, path[3].key), *nid);
        } else if (in_library && path.size() == 5 && (path[4].key == "functions" || path[4].key == "variables")) {
            const auto nid = parse_nid(v);
            if (!nid) {
                ok = false;
                error = Error{nx::format("line {}: {}", line_number, nid.error().message)};
                return;
            }
            pending.push_back(Pending{path[1].key, path[3].key, path[4].key == "functions" ? Kind::Function : Kind::Variable,
                                      nx::string(k), *nid});
        }
    });
    if (!ok) return nx::err(error);
    for (const auto& [lib, nid] : library_nids) add_library(nid, lib.second);
    for (const auto& p : pending) {
        const auto it = library_nids.find(std::pair(p.module, p.library));
        if (it == library_nids.end())
            return failure(nx::format("library '{}' of module '{}' has no NID", p.library, p.module));
        add(it->second, p.nid, p.kind, p.name);
    }
    return {};
}

Result<void> Database::load_tsv(nx::string_view text) {
    usize line_number = 0;
    Error error;
    bool ok = true;
    for_each_line(text, [&](nx::string_view line) {
        ++line_number;
        if (!ok || line.empty() || line.front() == '#') return;
        const auto f = split(line);
        const auto fail = [&](nx::string_view why) {
            ok = false;
            error = Error{nx::format("line {}: {}", line_number, why)};
        };
        if (f[0] == "L" && f.size() == 3) {
            const auto lib = parse_nid(f[1]);
            if (!lib) return fail(lib.error().message);
            add_library(*lib, f[2]);
        } else if ((f[0] == "F" || f[0] == "V") && f.size() == 4) {
            const auto lib = parse_nid(f[1]);
            const auto nid = parse_nid(f[2]);
            if (!lib || !nid) return fail("bad NID");
            add(*lib, *nid, f[0] == "F" ? Kind::Function : Kind::Variable, f[3]);
        } else {
            fail("expected L, F or V entry");
        }
    });
    if (!ok) return nx::err(error);
    return {};
}

nx::vector<Name> Database::entries() const {
    nx::vector<Name> out;
    out.reserve(names_.size());
    for (const auto& [k, n] : names_) out.push_back(n);
    std::sort(out.begin(), out.end(), [](const Name& a, const Name& b) { return key(a.library_nid, a.nid) < key(b.library_nid, b.nid); });
    return out;
}

nx::string Database::to_tsv() const {
    nx::vector<std::pair<u32, const nx::string*>> libs;
    for (const auto& [nid, name] : libraries_) libs.emplace_back(nid, &name);
    std::sort(libs.begin(), libs.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    nx::string out = "# psvrecore NID database: L library, F function, V variable (NIDs in hexadecimal)\n";
    for (const auto& [nid, name] : libs) out += nx::format("L\t{:08X}\t{}\n", nid, *name);
    for (const auto& n : entries())
        out += nx::format("{}\t{:08X}\t{:08X}\t{}\n", n.kind == Kind::Function ? "F" : "V", n.library_nid, n.nid, n.name);
    return out;
}

Result<void> Database::load(nx::fs::path_view path) {
    const auto load_file = [this](nx::fs::path_view file) -> Result<void> {
        const auto text = io::read_text_file(file);
        if (!text) return nx::err(text.error());
        const auto ext = file.extension();
        auto loaded = ext == ".yml" || ext == ".yaml" ? load_yaml(*text) : load_tsv(*text);
        if (!loaded) return nx::err(Error{nx::format("{}: {}", file.native(), loaded.error().message)});
        return {};
    };
    if (nx::fs::status(path) != nx::fs::file_type::directory) return load_file(path);
    const auto entries = nx::fs::directory_entries(path);
    if (!entries) return failure(nx::format("cannot list directory '{}'", path.native()));
    nx::vector<nx::string> files;
    for (const auto& e : entries.value()) {
        const auto ext = nx::fs::path_view(e.path).extension();
        if (e.type == nx::fs::file_type::regular && (ext == ".yml" || ext == ".yaml" || ext == ".tsv")) files.push_back(e.path);
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files)
        if (auto loaded = load_file(f); !loaded) return loaded;
    return {};
}

Result<StubNids> stub_archive_nids(std::span<const u8> archive) {
    const auto members = elf::read_archive(archive);
    if (!members) return nx::err(members.error());
    StubNids out;
    nx::map<nx::string, u32> library_nids;
    struct Pending {
        nx::string library;
        Name name;
    };
    nx::vector<Pending> pending;
    for (const auto& member : *members) {
        const auto obj = elf::File::parse(member.data);
        if (!obj) continue;  // (not an ELF object)
        // _F<nn>_ in the member's name: a function; _V<nn>_: a variable.
        const nx::string_view member_name = member.name;
        const bool is_variable = member_name.find("_V0") != nx::string_view::npos;
        for (const auto& sym : obj->symbols()) {
            nx::string_view s = sym.name;
            if (sym.section != elf::kSectionAbsolute || !s.starts_with("_INID_")) continue;
            s.remove_prefix(6);
            nx::string_view library, name;
            if (!take_counted(s, library)) continue;
            if (s.empty()) {
                library_nids.insert_or_assign(nx::string(library), sym.value);
                continue;
            }
            if (!s.starts_with("__")) continue;
            s.remove_prefix(2);
            if (!take_counted(s, name) || !s.empty()) continue;
            pending.push_back(Pending{nx::string(library),
                                      Name{0, sym.value, is_variable ? Kind::Variable : Kind::Function, nx::string(name)}});
        }
    }
    for (const auto& [name, nid] : library_nids) out.libraries.push_back(StubLibrary{name, nid});
    for (auto& p : pending) {
        const auto it = library_nids.find(p.library);
        if (it == library_nids.end()) return failure(nx::format("stub library '{}' has no library NID", p.library));
        p.name.library_nid = it->second;
        out.names.push_back(std::move(p.name));
    }
    return out;
}

}  // namespace psvrecore::nids
