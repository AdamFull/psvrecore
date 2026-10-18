#pragma once

// Text: numbers (the whole text must be the number: no locale, no partial parses) and the lines and fields of
// tab-separated lists.

#include <charconv>

#include "psvrecore/error.hpp"
#include "core/foundation/containers/small_vector.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/serialization/number_parse.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore {

// Base 0: decimal, or hexadecimal with a 0x prefix.
inline Result<u64> parse_u64(nx::string_view text, int base = 10) {
    const nx::string_view original = text;
    if (base == 0) {
        const bool hex = text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
        if (hex) text.remove_prefix(2);
        base = hex ? 16 : 10;
    }
    u64 value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value, base);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
        return failure(nx::format("not a number: '{}'", original));
    return value;
}

inline Result<i64> parse_i64(nx::string_view text, int base = 10) {
    const bool negative = !text.empty() && text[0] == '-';
    const auto magnitude = parse_u64(negative ? text.substr(1) : text, base);
    if (!magnitude) return nx::err(magnitude.error());
    constexpr u64 kMax = static_cast<u64>(INT64_MAX);
    if (*magnitude > kMax + (negative ? 1 : 0)) return failure(nx::format("number out of range: '{}'", text));
    return negative ? static_cast<i64>(0 - *magnitude) : static_cast<i64>(*magnitude);
}

inline Result<f64> parse_f64(nx::string_view text) {
    f64 value = 0;
    const auto [end, error] = nx::number::parse_f64(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || text.empty())
        return failure(nx::format("not a number: '{}'", text));
    return value;
}

// The fields of a line between `separator`s (empty fields kept).
inline nx::small_vector<nx::string_view, 8> split(nx::string_view line, char separator = '\t') {
    nx::small_vector<nx::string_view, 8> fields;
    for (usize start = 0;;) {
        const usize end = line.find(separator, start);
        fields.push_back(line.substr(start, end == nx::string_view::npos ? nx::string_view::npos : end - start));
        if (end == nx::string_view::npos) return fields;
        start = end + 1;
    }
}

// Calls `fn` with each line of `text` (without its \n or \r\n; a last line without \n included).
template <class Fn>
void for_each_line(nx::string_view text, Fn&& fn) {
    for (usize start = 0; start < text.size();) {
        usize end = text.find('\n', start);
        if (end == nx::string_view::npos) end = text.size();
        nx::string_view line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        fn(line);
        start = end + 1;
    }
}

}  // namespace psvrecore
