#pragma once

// NID database: the names of libraries, functions and variables, by NID. Sources:
// - vita-headers' NID database (db/<firmware>/*.yml, MIT): the system libraries' names.
// - Tab-separated files this module writes, for example built from an SDK's stub libraries on the user's machine
//   (stub_archive_nids). Those stay local: they come from SDK files.

#include <span>

#include "psvrecore/error.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::nids {

enum class Kind : u8 { Function, Variable };

struct Name {
    u32 library_nid = 0;
    u32 nid = 0;
    Kind kind = Kind::Function;
    nx::string name;
};

class Database {
public:
    void add_library(u32 library_nid, nx::string_view name);
    void add(u32 library_nid, u32 nid, Kind kind, nx::string_view name);

    // The name of a library, or null.
    const nx::string* library(u32 library_nid) const;
    // The name of a NID in a library, or null.
    const nx::string* find(u32 library_nid, u32 nid) const;

    usize library_count() const { return libraries_.size(); }
    usize size() const { return names_.size(); }

    // vita-headers' YAML (version 2: modules -> libraries -> functions / variables).
    Result<void> load_yaml(nx::string_view text);
    // Lines "L<TAB>library nid<TAB>name" and "F|V<TAB>library nid<TAB>nid<TAB>name" (NIDs in hexadecimal);
    // '#' starts a comment line.
    Result<void> load_tsv(nx::string_view text);
    nx::string to_tsv() const;

    // A .yml or .tsv file, or every such file in a directory.
    Result<void> load(nx::fs::path_view path);

    // Every entry, sorted by library and NID.
    nx::vector<Name> entries() const;

private:
    nx::flat_hash_map<u32, nx::string> libraries_;
    nx::flat_hash_map<u64, Name> names_;  // (library NID << 32) | NID
};

// The NIDs that an SDK stub library (an "ar" archive of ELF objects) declares, from its absolute symbols
// _INID_<n>_<library> (the library's NID) and _INID_<n>_<library>__<m>_<name> (a function's or variable's NID; the
// member's name tells which, _F.._ or _V.._). Skipped members (no such symbols) are not errors.
struct StubLibrary {
    nx::string name;
    u32 nid = 0;
};
struct StubNids {
    nx::vector<StubLibrary> libraries;
    nx::vector<Name> names;
};
Result<StubNids> stub_archive_nids(std::span<const u8> archive);

}  // namespace psvrecore::nids
