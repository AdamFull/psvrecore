// CPK archives and their @UTF tables, from synthetic files written here.

#include <cstring>
#include <variant>

#include "nxtest.h"
#include "psvrecore/formats/cpk.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

using namespace psvrecore;

namespace {

// --- a small @UTF writer ---

using Cell = std::variant<u64, nx::string, nx::vector<u8>>;

struct Col {
    nx::string name;
    cpk::Type type;
    cpk::Storage storage;
    nx::vector<Cell> cells;  // Constant: one; PerRow: one per row; Zero: none
};

struct Be {
    nx::vector<u8> d;
    void put(u64 v, usize n) {
        for (usize i = n; i-- > 0;) d.push_back(static_cast<u8>(v >> (8 * i)));
    }
    void put_at(usize at, u64 v, usize n) {
        for (usize i = 0; i < n; ++i) d[at + i] = static_cast<u8>(v >> (8 * (n - 1 - i)));
    }
};

usize size_of(cpk::Type t) {
    switch (t) {
        case cpk::Type::U8: case cpk::Type::S8: return 1;
        case cpk::Type::U16: case cpk::Type::S16: return 2;
        case cpk::Type::U64: case cpk::Type::S64: case cpk::Type::F64: case cpk::Type::Data: return 8;
        default: return 4;
    }
}

nx::vector<u8> utf_table(nx::string_view name, const nx::vector<Col>& cols, u32 rows) {
    nx::vector<u8> strings, data;
    const auto add_string = [&](nx::string_view s) {
        const u32 at = static_cast<u32>(strings.size());
        strings.insert(strings.end(), s.begin(), s.end());
        strings.push_back(0);
        return at;
    };
    add_string("<NULL>");
    const u32 name_at = add_string(name);
    const auto cell = [&](Be& out, cpk::Type t, const Cell& c) {
        if (t == cpk::Type::String) {
            out.put(add_string(std::get<nx::string>(c)), 4);
        } else if (t == cpk::Type::Data) {
            const auto& b = std::get<nx::vector<u8>>(c);
            out.put(data.size(), 4);
            out.put(b.size(), 4);
            data.insert(data.end(), b.begin(), b.end());
        } else {
            out.put(std::get<u64>(c), size_of(t));
        }
    };
    Be columns, row_bytes;
    usize row_size = 0;
    for (const auto& c : cols) {
        const u8 flags = static_cast<u8>(0x10 | static_cast<u8>(c.type) |
                                         (c.storage == cpk::Storage::Constant ? 0x20 : 0) |
                                         (c.storage == cpk::Storage::PerRow ? 0x40 : 0));
        columns.put(flags, 1);
        columns.put(add_string(c.name), 4);
        if (c.storage == cpk::Storage::Constant) cell(columns, c.type, c.cells[0]);
        if (c.storage == cpk::Storage::PerRow) row_size += size_of(c.type);
    }
    for (u32 r = 0; r < rows; ++r)
        for (const auto& c : cols)
            if (c.storage == cpk::Storage::PerRow) cell(row_bytes, c.type, c.cells[r]);

    Be t;
    t.put(cpk::kUtfMagic, 4);
    t.put(0, 4);  // size, below
    const usize rows_at = 32 + columns.d.size();
    const usize strings_at = rows_at + row_bytes.d.size();
    const usize data_at = strings_at + strings.size();
    t.put(0, 2);
    t.put(rows_at - 8, 2);
    t.put(strings_at - 8, 4);
    t.put(data_at - 8, 4);
    t.put(name_at, 4);
    t.put(cols.size(), 2);
    t.put(row_size, 2);
    t.put(rows, 4);
    t.d.insert(t.d.end(), columns.d.begin(), columns.d.end());
    t.d.insert(t.d.end(), row_bytes.d.begin(), row_bytes.d.end());
    t.d.insert(t.d.end(), strings.begin(), strings.end());
    t.d.insert(t.d.end(), data.begin(), data.end());
    t.put_at(4, t.d.size() - 8, 4);
    return t.d;
}

// A chunk (tag, flags, size of the table) and its table at `at` in `out`.
void put_chunk(nx::vector<u8>& out, usize at, nx::string_view tag, const nx::vector<u8>& table) {
    if (out.size() < at + 16 + table.size()) out.resize(at + 16 + table.size(), 0);
    std::memcpy(out.data() + at, tag.data(), 4);
    out[at + 4] = 0xFF;
    const u64 size = table.size();
    std::memcpy(out.data() + at + 8, &size, 8);
    std::memcpy(out.data() + at + 16, table.data(), table.size());
}

Col per_row(nx::string name, cpk::Type t, nx::vector<Cell> cells) { return {std::move(name), t, cpk::Storage::PerRow, std::move(cells)}; }
Col constant(nx::string name, cpk::Type t, Cell c) { return {std::move(name), t, cpk::Storage::Constant, {std::move(c)}}; }

nx::vector<u8> header_table(u64 content, u64 toc, u32 files) {
    return utf_table("CpkHeader",
                     {per_row("UpdateDateTime", cpk::Type::U64, {u64{1}}), per_row("ContentOffset", cpk::Type::U64, {content}),
                      per_row("TocOffset", cpk::Type::U64, {toc}), per_row("Files", cpk::Type::U32, {u64{files}}),
                      {"EtocOffset", cpk::Type::U64, cpk::Storage::Zero, {}}, per_row("Align", cpk::Type::U16, {u64{0x800}}),
                      per_row("Tvers", cpk::Type::String, {nx::string("CPKMC2.18.04, DLL2.78.03")})},
                     1);
}

struct Memory {
    const nx::vector<u8>& d;
    bool operator()(u64 offset, void* out, usize size) const {
        if (offset > d.size() || size > d.size() - offset) return false;
        std::memcpy(out, d.data() + offset, size);
        return true;
    }
};

}  // namespace

TEST_CASE("cpk: @UTF columns stored as zero, once, or in each row") {
    const auto bytes = utf_table(
        "Sample",
        {per_row("Byte", cpk::Type::U8, {u64{7}, u64{200}}), per_row("Short", cpk::Type::S16, {u64{0xFFFE}, u64{3}}),
         constant("Same", cpk::Type::U32, u64{0xDEADBEEF}), {"Nothing", cpk::Type::U64, cpk::Storage::Zero, {}},
         per_row("Big", cpk::Type::U64, {u64{0x1'0000'0002}, u64{5}}), per_row("Real", cpk::Type::F32, {u64{0x3FC00000}, u64{0}}),
         per_row("Name", cpk::Type::String, {nx::string("first"), nx::string("second")}),
         per_row("Blob", cpk::Type::Data, {nx::vector<u8>{1, 2, 3}, nx::vector<u8>{}})},
        2);
    const auto t = cpk::parse_table(bytes);
    REQUIRE(t.has_value());
    CHECK(t->name == "Sample");
    REQUIRE(t->columns.size() == 8);
    REQUIRE(t->rows.size() == 2);
    CHECK(t->columns[2].storage == cpk::Storage::Constant);
    CHECK(t->columns[3].storage == cpk::Storage::Zero);
    CHECK(t->integer(0, "Byte") == 7u);
    CHECK(t->integer(1, "Byte") == 200u);
    CHECK(static_cast<i64>(*t->integer(0, "Short")) == -2);
    CHECK(t->integer(1, "Same") == 0xDEADBEEFu);
    CHECK(t->integer(1, "Nothing") == 0u);
    CHECK(t->integer(0, "Big") == 0x1'0000'0002u);
    CHECK(std::get<double>(t->rows[0][5]) == 1.5);
    CHECK(t->text(1, "Name") == nx::string_view("second"));
    const auto blob = t->data(0, "Blob");
    REQUIRE(blob.has_value());
    REQUIRE(blob->size() == 3);
    CHECK((*blob)[2] == 3);
    CHECK(t->data(1, "Blob")->empty());
    CHECK_FALSE(t->integer(0, "Name").has_value());
    CHECK_FALSE(t->integer(0, "Missing").has_value());
    CHECK_FALSE(t->integer(2, "Byte").has_value());
}

TEST_CASE("cpk: damaged @UTF tables are errors") {
    const auto good = utf_table("T", {per_row("Name", cpk::Type::String, {nx::string("a")})}, 1);
    REQUIRE(cpk::parse_table(good).has_value());
    {
        auto bad = good;
        bad[0] = 'X';
        CHECK_FALSE(cpk::parse_table(bad).has_value());
    }
    {
        auto bad = good;
        bad[7] += 1;  // one byte more than there is
        CHECK_FALSE(cpk::parse_table(bad).has_value());
    }
    {
        auto bad = good;
        bad[31] = 50;  // 50 rows
        CHECK_FALSE(cpk::parse_table(bad).has_value());
    }
    {
        auto bad = good;
        const usize row = (usize{bad[10]} << 8 | bad[11]) + 8;
        bad[row] = 0x7F;  // the row's string far outside
        CHECK_FALSE(cpk::parse_table(bad).has_value());
    }
    {
        auto bad = good;
        bad[32] = 0x5C;  // a column of type 0xC
        CHECK_FALSE(cpk::parse_table(bad).has_value());
    }
    CHECK_FALSE(cpk::parse_table(nx::vector<u8>{0x40, 0x55}).has_value());
}

TEST_CASE("cpk: an archive's files from its table of contents") {
    constexpr u64 kToc = 0x800, kContent = 0x1000;
    nx::vector<u8> archive(0x2000, 0);
    put_chunk(archive, 0, "CPK ", header_table(kContent, kToc, 2));
    // Offsets count from the table of contents, which comes before the content here.
    put_chunk(archive, kToc, "TOC ",
              utf_table("CpkTocInfo",
                        {per_row("DirName", cpk::Type::String, {nx::string(""), nx::string("sound/bgm")}),
                         per_row("FileName", cpk::Type::String, {nx::string("boot.bin"), nx::string("song.bin")}),
                         per_row("FileSize", cpk::Type::U32, {u64{0x10}, u64{0x20}}),
                         per_row("ExtractSize", cpk::Type::U32, {u64{0x10}, u64{0x80}}),
                         per_row("FileOffset", cpk::Type::U64, {u64{kContent - kToc}, u64{kContent + 0x800 - kToc}}),
                         per_row("ID", cpk::Type::U32, {u64{0}, u64{1}})},
                        2));
    std::memcpy(archive.data() + kContent + 0x800, "CRILAYLA", 8);
    const auto a = cpk::read_archive(archive.size(), Memory{archive});
    REQUIRE(a.has_value());
    CHECK(a->content_offset == kContent);
    CHECK(a->toc_offset == kToc);
    CHECK(a->etoc_offset == 0u);
    CHECK(a->align == 0x800u);
    CHECK(a->declared_files == 2u);
    CHECK(a->tool_version == "CPKMC2.18.04, DLL2.78.03");
    REQUIRE(a->files.size() == 2);
    CHECK(a->files[0].path() == "boot.bin");
    CHECK(a->files[0].offset == kContent);
    CHECK_FALSE(a->files[0].compressed());
    CHECK(a->files[1].path() == "sound/bgm/song.bin");
    CHECK(a->files[1].offset == kContent + 0x800);
    CHECK(a->files[1].compressed());
    CHECK(a->files[1].id == 1u);
}

TEST_CASE("cpk: archives with a missing chunk or a file outside are errors") {
    nx::vector<u8> archive(0x1000, 0);
    put_chunk(archive, 0, "CPK ", header_table(0, 0x800, 1));
    CHECK_FALSE(cpk::read_archive(archive.size(), Memory{archive}).has_value());  // no "TOC " at 0x800
    put_chunk(archive, 0x800, "TOC ",
              utf_table("CpkTocInfo",
                        {per_row("DirName", cpk::Type::String, {nx::string("")}),
                         per_row("FileName", cpk::Type::String, {nx::string("far.bin")}),
                         per_row("FileSize", cpk::Type::U32, {u64{0x100}}),
                         per_row("FileOffset", cpk::Type::U64, {u64{0x7F0}})},
                        1));
    CHECK_FALSE(cpk::read_archive(archive.size(), Memory{archive}).has_value());  // ends past 0x1000
    archive.resize(0x1100);
    const auto a = cpk::read_archive(archive.size(), Memory{archive});
    REQUIRE(a.has_value());
    CHECK(a->files[0].extract_size == 0x100u);  // no ExtractSize column: as stored
    CHECK_FALSE(cpk::is_cpk(nx::vector<u8>{'C', 'P', 'K'}));
    CHECK(cpk::is_cpk(archive));
    archive[0] = 'X';
    CHECK_FALSE(cpk::read_archive(archive.size(), Memory{archive}).has_value());
}
