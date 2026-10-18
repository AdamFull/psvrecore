#pragma once

// Errors. A failure the caller expects and handles (a missing file, data in a format it does not know) comes back
// as a Result: an nx::result holding the value or an Error. A failure nothing can recover from is an Exception,
// caught at the top (the front end, the tools) where it ends the program with its message.

#include <stdexcept>

#include "core/foundation/core/result.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore {

struct Error {
    nx::string message;
};

template <class T>
using Result = nx::result<T, Error>;

// The error side of a Result: `return failure("not an ELF file");`
inline nx::err<Error> failure(nx::string message) { return nx::err<Error>(Error{std::move(message)}); }

struct Exception : std::runtime_error {
    using std::runtime_error::runtime_error;
    explicit Exception(nx::string_view message) : std::runtime_error(nx::string(message).c_str()) {}
    explicit Exception(const Error& error) : std::runtime_error(error.message.c_str()) {}
};

// The value of a Result, or an Exception with its error (where a failure ends the program anyway).
template <class T>
T value_or_throw(Result<T>&& result) {
    if (!result) throw Exception(result.error());
    if constexpr (!std::is_void_v<T>) return std::move(*result);
}

}  // namespace psvrecore
