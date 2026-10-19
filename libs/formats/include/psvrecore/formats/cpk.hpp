#pragma once

// CPK: CRI Middleware's file archive, read by the game's own code (CRI File System) through the Vita's file API. The
// runtime does not need to open one; this is for inspecting game data (psvinfo cpk) and for tests.
//
// A CPK is a sequence of chunks ("CPK ", "TOC ", "ITOC", "ETOC", "GTOC"), each a 16-byte header (tag, flags, size
// of what follows) and an @UTF table: a small big-endian table with named, typed columns. The "CPK " table at the
// start gives where the others are; "TOC " lists the files by folder and name, "ITOC" by number. A file stored
// compressed (CRILAYLA) has an extracted size larger than its stored size.

#include <optional>
#include <span>
#include <variant>

#include "psvrecore/error.hpp"
#include "core/foundation/core/callable.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::cpk {

// --- @UTF tables ---

enum class Type : u8 {
    U8 = 0x0,
    S8 = 0x1,
    U16 = 0x2,
    S16 = 0x3,
    U32 = 0x4,
    S32 = 0x5,
    U64 = 0x6,
    S64 = 0x7,
    F32 = 0x8,
    F64 = 0x9,
    String = 0xA,
    Data = 0xB,
};

// Where a column's values are: none (every row zero or empty), once in the column (the same for every row), or in
// each row.
enum class Storage : u8 { Zero, Constant, PerRow };

// A data value: bytes of the table (its data pool), as an offset and size from the table's start.
struct DataRef {
    u32 offset = 0, size = 0;
};

using Value = std::variant<std::monostate, u64, i64, double, nx::string, DataRef>;

struct Column {
    nx::string name;
    Type type = Type::U8;
    Storage storage = Storage::Zero;
    Value constant;  // Storage::Constant
};

struct Table {
    nx::string name;
    nx::vector<Column> columns;
    nx::vector<nx::vector<Value>> rows;  // one value per column (constant and zero columns included)
    nx::vector<u8> bytes;                // the table as read (DataRef points into it)

    std::optional<usize> column(nx::string_view name) const;
    // A row's value as an integer (of any integer type, or zero), a string or bytes; nothing if the column is
    // missing or of another kind.
    std::optional<u64> integer(usize row, nx::string_view column) const;
    std::optional<nx::string_view> text(usize row, nx::string_view column) const;
    std::optional<std::span<const u8>> data(usize row, nx::string_view column) const;
};

inline constexpr u32 kUtfMagic = 0x40555446;  // "@UTF", big-endian

// An @UTF table at the start of `bytes` (copied into the Table).
Result<Table> parse_table(std::span<const u8> bytes);

// --- archives ---

// Reads `size` bytes at `offset` of the archive into `out` (false if they are not all there).
using ReadAt = nx::function_ref<bool(u64 offset, void* out, usize size)>;

struct Entry {
    nx::string folder;  // "" at the top
    nx::string name;
    u64 offset = 0;     // from the start of the archive
    u64 size = 0;       // stored
    u64 extract_size = 0;
    std::optional<u32> id;
    bool compressed() const { return extract_size > size; }
    nx::string path() const;  // folder/name
};

struct Archive {
    Table header;  // the "CPK " table
    u64 content_offset = 0, toc_offset = 0, itoc_offset = 0, etoc_offset = 0, gtoc_offset = 0;
    u32 align = 0;
    u32 declared_files = 0;  // the header's file count
    nx::string tool_version; // "Tvers"
    nx::vector<Entry> files;
    bool has_toc() const { return toc_offset != 0; }
};

inline constexpr u32 kChunkHeaderSize = 0x10;

bool is_cpk(std::span<const u8> start);
// An archive of `size` bytes: its header and file lists (no file is read).
Result<Archive> read_archive(u64 size, ReadAt read);

}  // namespace psvrecore::cpk
