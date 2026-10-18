#pragma once

// Binary data in file formats: ByteReader reads little- and big-endian values with bounds checks, ByteWriter
// appends them.

#include <bit>
#include <cstring>
#include <span>
#include <type_traits>

#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::io {

// The bytes of an integer in reverse order (C++20 has no std::byteswap; nx foundation has no scalar one).
template <class T>
    requires std::is_integral_v<T>
constexpr T byte_swap(T value) {
    using U = std::make_unsigned_t<T>;
    U in = static_cast<U>(value), out = 0;
    for (usize i = 0; i < sizeof(T); ++i, in >>= 8) out = static_cast<U>((out << 8) | (in & 0xFF));
    return static_cast<T>(out);
}

// The bytes of trivially copyable values (e.g. to hash or write pixels).
template <class T>
    requires std::is_trivially_copyable_v<T>
std::span<const u8> bytes_of(std::span<const T> values) {
    return {reinterpret_cast<const u8*>(values.data()), values.size_bytes()};
}

// A copy of bytes as a vector (e.g. a file read whole, kept in a structure).
inline nx::vector<u8> to_vector(std::span<const u8> bytes) { return nx::vector<u8>(bytes.begin(), bytes.end()); }

// Reads from a span the caller keeps alive. A read outside it returns zero (an empty span or string) and marks the
// reader as failed, so a parser checks failed() once at the end instead of after every read.
class ByteReader {
public:
    ByteReader() = default;
    explicit ByteReader(std::span<const u8> data) : data_(data) {}

    std::span<const u8> data() const { return data_; }
    usize size() const { return data_.size(); }
    bool failed() const { return failed_; }
    bool contains(usize at, usize size) const { return at <= data_.size() && size <= data_.size() - at; }

    template <class T>
        requires std::is_trivially_copyable_v<T>
    T le(usize at) const {
        T value{};
        if (!check(at, sizeof(T))) return value;
        std::memcpy(&value, data_.data() + at, sizeof(T));
        if constexpr (std::endian::native == std::endian::big && std::is_integral_v<T>) value = byte_swap(value);
        return value;
    }
    template <class T>
        requires std::is_integral_v<T>
    T be(usize at) const {
        T value{};
        if (!check(at, sizeof(T))) return value;
        std::memcpy(&value, data_.data() + at, sizeof(T));
        if constexpr (std::endian::native == std::endian::little) value = byte_swap(value);
        return value;
    }
    u8 byte(usize at) const { return check(at, 1) ? data_[at] : u8{0}; }

    // `size` bytes from `at` (empty, and failed, if they are not all there).
    std::span<const u8> slice(usize at, usize size) const {
        return check(at, size) ? data_.subspan(at, size) : std::span<const u8>{};
    }
    // The text from `at` up to a NUL, the end of the data or `max` characters.
    nx::string cstr(usize at, usize max = ~usize{0}) const {
        if (!check(at, 0)) return {};
        const usize end = at + nx::min(max, data_.size() - at);
        usize n = at;
        while (n < end && data_[n] != 0) ++n;
        return nx::string(reinterpret_cast<const char*>(data_.data()) + at, n - at);
    }
    // Starts with these bytes?
    bool matches(usize at, std::span<const u8> bytes) const {
        return contains(at, bytes.size()) && std::memcmp(data_.data() + at, bytes.data(), bytes.size()) == 0;
    }
    bool matches(usize at, nx::string_view text) const {
        return matches(at, std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
    }

private:
    bool check(usize at, usize size) const {
        if (contains(at, size)) return true;
        failed_ = true;
        return false;
    }

    std::span<const u8> data_;
    mutable bool failed_ = false;
};

// Appends little-endian values to a byte vector.
class ByteWriter {
public:
    explicit ByteWriter(nx::vector<u8>& out) : out_(out) {}

    usize size() const { return out_.size(); }
    void bytes(std::span<const u8> data) { out_.insert(out_.end(), data.begin(), data.end()); }
    void text(nx::string_view s) { bytes(std::span(reinterpret_cast<const u8*>(s.data()), s.size())); }
    void zeros(usize n) { out_.resize(out_.size() + n, 0); }
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void le(T value) {
        const usize at = out_.size();
        out_.resize(at + sizeof(T));
        put(at, value);
    }
    // Overwrites a value written before (e.g. a size known only at the end).
    template <class T>
        requires std::is_trivially_copyable_v<T>
    void patch_le(usize at, T value) {
        put(at, value);
    }

private:
    template <class T>
    void put(usize at, T value) {
        if constexpr (std::endian::native == std::endian::big && std::is_integral_v<T>) value = byte_swap(value);
        std::memcpy(out_.data() + at, &value, sizeof(T));
    }

    nx::vector<u8>& out_;
};

}  // namespace psvrecore::io
