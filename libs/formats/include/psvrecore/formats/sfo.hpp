#pragma once

// SFO ("system file object", e.g. sce_sys/param.sfo): a table of named values (title, title ID, version, ...),
// each a UTF-8 string or a 32-bit integer.

#include <optional>
#include <span>

#include "psvrecore/error.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::sfo {

inline constexpr u32 kMagic = 0x46535000;  // "\0PSF"

enum class Format : u16 {
    Utf8Special = 0x0004,  // UTF-8, not NUL-terminated
    Utf8 = 0x0204,         // UTF-8, NUL-terminated
    Int32 = 0x0404,
};

struct Entry {
    nx::string key;
    Format format = Format::Utf8;
    u32 length = 0;      // bytes used
    u32 max_length = 0;  // bytes reserved
    nx::string text;     // Utf8 / Utf8Special
    u32 integer = 0;     // Int32
};

struct File {
    u32 version = 0;
    nx::vector<Entry> entries;

    const Entry* find(nx::string_view key) const;
    // The value of a text entry, or nothing (missing, or not text).
    std::optional<nx::string_view> text(nx::string_view key) const;
    std::optional<u32> integer(nx::string_view key) const;
};

bool is_sfo(std::span<const u8> data);
Result<File> parse(std::span<const u8> data);

}  // namespace psvrecore::sfo
