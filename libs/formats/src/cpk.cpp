#include "psvrecore/formats/cpk.hpp"

#include "psvrecore/io/bytes.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::cpk {

namespace {

// Column flags: the low nibble is the type; a name, a value in the column, a value in each row.
constexpr u8 kTypeMask = 0x0F, kHasName = 0x10, kHasConstant = 0x20, kPerRow = 0x40;
constexpr usize kTableHeaderSize = 32;
// The largest chunk read whole (the tables of the largest archives are a few megabytes).
constexpr u64 kMaxChunk = 256ull << 20;

usize type_size(Type t) {
    switch (t) {
        case Type::U8: case Type::S8: return 1;
        case Type::U16: case Type::S16: return 2;
        case Type::U32: case Type::S32: case Type::F32: case Type::String: return 4;
        case Type::U64: case Type::S64: case Type::F64: case Type::Data: return 8;
    }
    return 0;
}

bool known_type(u8 t) { return t <= static_cast<u8>(Type::Data); }

Value zero_value(Type t) {
    switch (t) {
        case Type::S8: case Type::S16: case Type::S32: case Type::S64: return i64{0};
        case Type::F32: case Type::F64: return 0.0;
        case Type::String: return nx::string();
        case Type::Data: return DataRef{};
        default: return u64{0};
    }
}

// Where the pools are, from the table's start.
struct Pools {
    usize strings = 0, data = 0, end = 0;
};

Result<Value> read_value(const io::ByteReader& d, usize at, Type t, const Pools& p) {
    switch (t) {
        case Type::U8: return Value(u64{d.byte(at)});
        case Type::S8: return Value(i64{static_cast<i8>(d.byte(at))});
        case Type::U16: return Value(u64{d.be<u16>(at)});
        case Type::S16: return Value(i64{static_cast<i16>(d.be<u16>(at))});
        case Type::U32: return Value(u64{d.be<u32>(at)});
        case Type::S32: return Value(i64{static_cast<i32>(d.be<u32>(at))});
        case Type::U64: return Value(d.be<u64>(at));
        case Type::S64: return Value(static_cast<i64>(d.be<u64>(at)));
        case Type::F32: return Value(double{std::bit_cast<float>(d.be<u32>(at))});
        case Type::F64: return Value(std::bit_cast<double>(d.be<u64>(at)));
        case Type::String: {
            const usize s = p.strings + d.be<u32>(at);
            if (s >= p.end) return failure(nx::format("string at 0x{:x} outside the table", s));
            return Value(d.cstr(s, p.end - s));
        }
        case Type::Data: {
            const u32 offset = d.be<u32>(at), size = d.be<u32>(at + 4);
            const usize s = p.data + offset;
            if (size && (s > p.end || size > p.end - s)) return failure(nx::format("data at 0x{:x} outside the table", s));
            return Value(DataRef{size ? static_cast<u32>(s) : 0u, size});
        }
    }
    return failure("unknown column type");
}

}  // namespace

std::optional<usize> Table::column(nx::string_view name) const {
    for (usize i = 0; i < columns.size(); ++i)
        if (columns[i].name == name) return i;
    return std::nullopt;
}

std::optional<u64> Table::integer(usize row, nx::string_view name) const {
    const auto c = column(name);
    if (!c || row >= rows.size()) return std::nullopt;
    const Value& v = rows[row][*c];
    if (const u64* u = std::get_if<u64>(&v)) return *u;
    if (const i64* s = std::get_if<i64>(&v)) return static_cast<u64>(*s);
    return std::nullopt;
}

std::optional<nx::string_view> Table::text(usize row, nx::string_view name) const {
    const auto c = column(name);
    if (!c || row >= rows.size()) return std::nullopt;
    if (const nx::string* s = std::get_if<nx::string>(&rows[row][*c])) return s->view();
    return std::nullopt;
}

std::optional<std::span<const u8>> Table::data(usize row, nx::string_view name) const {
    const auto c = column(name);
    if (!c || row >= rows.size()) return std::nullopt;
    if (const DataRef* r = std::get_if<DataRef>(&rows[row][*c]))
        return std::span<const u8>(bytes).subspan(r->offset, r->size);
    return std::nullopt;
}

Result<Table> parse_table(std::span<const u8> bytes) {
    const io::ByteReader head(bytes);
    if (bytes.size() < kTableHeaderSize || head.be<u32>(0) != kUtfMagic) return failure("not an @UTF table");
    const u64 total = u64{head.be<u32>(4)} + 8;
    if (total > bytes.size()) return failure(nx::format("@UTF table of 0x{:x} bytes in 0x{:x}", total, bytes.size()));
    Table t;
    t.bytes.assign(bytes.begin(), bytes.begin() + static_cast<isize>(total));
    const io::ByteReader d(t.bytes);
    // Offsets in the header count from just after the size.
    const usize rows_at = usize{d.be<u16>(10)} + 8;
    Pools p{usize{d.be<u32>(12)} + 8, usize{d.be<u32>(16)} + 8, t.bytes.size()};
    const u32 name = d.be<u32>(20);
    const u16 column_count = d.be<u16>(24), row_size = d.be<u16>(26);
    const u32 row_count = d.be<u32>(28);
    if (p.strings > p.end || p.data > p.end || rows_at > p.strings)
        return failure("@UTF table: pools out of order or outside the table");
    if (p.strings + name >= p.end) return failure("@UTF table: name outside the table");
    t.name = d.cstr(p.strings + name, p.end - p.strings - name);

    usize at = kTableHeaderSize, per_row = 0;
    for (u16 i = 0; i < column_count; ++i) {
        if (!d.contains(at, 5)) return failure(nx::format("@UTF table '{}': column {} outside the table", t.name, i));
        const u8 flags = d.byte(at);
        if (!known_type(flags & kTypeMask))
            return failure(nx::format("@UTF table '{}': column {} of unknown type 0x{:x}", t.name, i, flags & kTypeMask));
        Column c;
        c.type = static_cast<Type>(flags & kTypeMask);
        if (flags & kHasName) {
            const usize s = p.strings + d.be<u32>(at + 1);
            if (s >= p.end) return failure(nx::format("@UTF table '{}': column {} name outside the table", t.name, i));
            c.name = d.cstr(s, p.end - s);
            at += 5;
        } else {
            at += 1;
        }
        c.constant = zero_value(c.type);
        if (flags & kHasConstant) {
            auto v = read_value(d, at, c.type, p);
            if (!v) return nx::err(v.error());
            c.constant = std::move(*v);
            at += type_size(c.type);
        }
        c.storage = flags & kPerRow ? Storage::PerRow : flags & kHasConstant ? Storage::Constant : Storage::Zero;
        if (c.storage == Storage::PerRow) per_row += type_size(c.type);
        t.columns.push_back(std::move(c));
    }
    if (at > rows_at) return failure(nx::format("@UTF table '{}': columns overlap the rows", t.name));
    if (per_row > row_size)
        return failure(nx::format("@UTF table '{}': values of {} bytes in rows of {}", t.name, per_row, row_size));
    if (u64{row_count} * row_size > p.strings - rows_at)
        return failure(nx::format("@UTF table '{}': {} rows outside the table", t.name, row_count));

    t.rows.reserve(row_count);
    for (u32 r = 0; r < row_count; ++r) {
        nx::vector<Value> row;
        row.reserve(t.columns.size());
        usize pos = rows_at + usize{r} * row_size;
        for (const auto& c : t.columns) {
            if (c.storage != Storage::PerRow) {
                row.push_back(c.constant);
                continue;
            }
            auto v = read_value(d, pos, c.type, p);
            if (!v) return failure(nx::format("@UTF table '{}' row {} column '{}': {}", t.name, r, c.name, v.error().message));
            row.push_back(std::move(*v));
            pos += type_size(c.type);
        }
        t.rows.push_back(std::move(row));
    }
    if (d.failed()) return failure(nx::format("@UTF table '{}': read outside the table", t.name));
    return t;
}

nx::string Entry::path() const { return folder.empty() ? name : nx::format("{}/{}", folder, name); }

bool is_cpk(std::span<const u8> start) { return io::ByteReader(start).matches(0, "CPK "); }

namespace {

// The table of the chunk at `offset`, which must carry `tag`.
Result<Table> read_chunk(u64 archive_size, ReadAt read, u64 offset, nx::string_view tag) {
    u8 head[kChunkHeaderSize];
    if (offset > archive_size || archive_size - offset < kChunkHeaderSize || !read(offset, head, sizeof head))
        return failure(nx::format("'{}' chunk at 0x{:x} outside the archive", tag, offset));
    const io::ByteReader h(head);
    if (!h.matches(0, tag)) return failure(nx::format("no '{}' chunk at 0x{:x}", tag, offset));
    const u64 size = h.le<u64>(8);
    if (size > kMaxChunk || size > archive_size - offset - kChunkHeaderSize)
        return failure(nx::format("'{}' chunk at 0x{:x}: size 0x{:x} too large", tag, offset, size));
    nx::vector<u8> bytes(static_cast<usize>(size));
    if (!read(offset + kChunkHeaderSize, bytes.data(), bytes.size()))
        return failure(nx::format("'{}' chunk at 0x{:x} could not be read", tag, offset));
    auto table = parse_table(bytes);
    if (!table) return failure(nx::format("'{}' chunk: {}", tag, table.error().message));
    return table;
}

}  // namespace

Result<Archive> read_archive(u64 size, ReadAt read) {
    Archive a;
    {
        auto header = read_chunk(size, read, 0, "CPK ");
        if (!header) return nx::err(header.error());
        a.header = std::move(*header);
    }
    if (a.header.rows.size() != 1) return failure(nx::format("CPK header table of {} rows", a.header.rows.size()));
    const auto get = [&](nx::string_view name) { return a.header.integer(0, name).value_or(0); };
    a.content_offset = get("ContentOffset");
    a.toc_offset = get("TocOffset");
    a.itoc_offset = get("ItocOffset");
    a.etoc_offset = get("EtocOffset");
    a.gtoc_offset = get("GtocOffset");
    a.align = static_cast<u32>(get("Align"));
    a.declared_files = static_cast<u32>(get("Files"));
    a.tool_version = nx::string(a.header.text(0, "Tvers").value_or(""));

    if (!a.has_toc()) {
        if (a.itoc_offset) return failure("archive lists its files by number only (ITOC), which is not read yet");
        return a;  // no files
    }
    auto toc = read_chunk(size, read, a.toc_offset, "TOC ");
    if (!toc) return nx::err(toc.error());
    // File offsets count from the table of contents, or from the content if that comes first.
    const u64 base = a.content_offset ? nx::min(a.content_offset, a.toc_offset) : a.toc_offset;
    a.files.reserve(toc->rows.size());
    for (usize r = 0; r < toc->rows.size(); ++r) {
        Entry e;
        e.folder = nx::string(toc->text(r, "DirName").value_or(""));
        e.name = nx::string(toc->text(r, "FileName").value_or(""));
        const auto offset = toc->integer(r, "FileOffset");
        const auto stored = toc->integer(r, "FileSize");
        if (!offset || !stored) return failure(nx::format("TOC row {}: no FileOffset or FileSize", r));
        e.offset = base + *offset;
        e.size = *stored;
        e.extract_size = toc->integer(r, "ExtractSize").value_or(e.size);
        if (const auto id = toc->integer(r, "ID")) e.id = static_cast<u32>(*id);
        if (e.offset > size || e.size > size - e.offset)
            return failure(nx::format("'{}': 0x{:x} bytes at 0x{:x}, outside the archive", e.path(), e.size, e.offset));
        a.files.push_back(std::move(e));
    }
    return a;
}

}  // namespace psvrecore::cpk
