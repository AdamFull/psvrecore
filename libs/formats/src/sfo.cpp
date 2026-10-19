#include "psvrecore/formats/sfo.hpp"

#include <optional>

#include "psvrecore/io/bytes.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::sfo {

namespace {

constexpr usize kHeaderSize = 20, kIndexEntrySize = 16;

}  // namespace

const Entry* File::find(nx::string_view key) const {
    for (const auto& e : entries)
        if (e.key == key) return &e;
    return nullptr;
}

std::optional<nx::string_view> File::text(nx::string_view key) const {
    const Entry* e = find(key);
    if (!e || e->format == Format::Int32) return std::nullopt;
    return e->text.view();
}

std::optional<u32> File::integer(nx::string_view key) const {
    const Entry* e = find(key);
    if (!e || e->format != Format::Int32) return std::nullopt;
    return e->integer;
}

bool is_sfo(std::span<const u8> data) { return io::ByteReader(data).le<u32>(0) == kMagic && data.size() >= kHeaderSize; }

Result<File> parse(std::span<const u8> bytes) {
    const io::ByteReader d(bytes);
    if (!is_sfo(bytes)) return failure("not an SFO file");
    File f;
    f.version = d.le<u32>(4);
    const u32 key_table = d.le<u32>(8), data_table = d.le<u32>(12), count = d.le<u32>(16);
    if (!d.contains(kHeaderSize, usize{count} * kIndexEntrySize)) return failure("SFO index out of bounds");
    f.entries.reserve(count);
    for (usize i = 0; i < count; ++i) {
        const usize at = kHeaderSize + i * kIndexEntrySize;
        Entry e;
        const u16 key_offset = d.le<u16>(at);
        e.format = static_cast<Format>(d.le<u16>(at + 2));
        e.length = d.le<u32>(at + 4);
        e.max_length = d.le<u32>(at + 8);
        const usize value = usize{data_table} + d.le<u32>(at + 12);
        if (!d.contains(usize{key_table} + key_offset, 1)) return failure(nx::format("SFO entry {}: key out of bounds", i));
        e.key = d.cstr(usize{key_table} + key_offset);
        if (e.length > e.max_length || !d.contains(value, e.max_length))
            return failure(nx::format("SFO entry '{}': value out of bounds", e.key));
        switch (e.format) {
            case Format::Int32:
                if (e.length != 4) return failure(nx::format("SFO entry '{}': integer of {} bytes", e.key, e.length));
                e.integer = d.le<u32>(value);
                break;
            case Format::Utf8: e.text = d.cstr(value, e.length); break;
            case Format::Utf8Special: {
                const auto raw = d.slice(value, e.length);
                e.text = nx::string(reinterpret_cast<const char*>(raw.data()), raw.size());
                break;
            }
            default:
                return failure(nx::format("SFO entry '{}': unknown format 0x{:04x}", e.key, static_cast<u16>(e.format)));
        }
        f.entries.push_back(std::move(e));
    }
    return f;
}

}  // namespace psvrecore::sfo
