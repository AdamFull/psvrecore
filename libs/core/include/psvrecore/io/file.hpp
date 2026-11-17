#pragma once

// Whole files through nx::fs. Failures come back as Errors that name the file and the reason. Writes are atomic: a
// file is replaced only once its new contents are complete, so extracted or generated output is never left half
// written.

#include <span>

#include "psvrecore/abi.h"
#include "psvrecore/error.hpp"
#include "core/foundation/core/foundation.h"
#include "core/foundation/containers/blob.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::io {

PSVRECORE_API Result<nx::blob<u8>> read_file(nx::fs::path_view path);
PSVRECORE_API Result<nx::string> read_text_file(nx::fs::path_view path);
PSVRECORE_API Result<void> write_file(nx::fs::path_view path, std::span<const u8> data);
PSVRECORE_API Result<void> write_text_file(nx::fs::path_view path, nx::string_view text);

// The bytes of a blob as a span (the blob keeps them).
inline std::span<const u8> view(const nx::blob<u8>& data) { return {data.data(), data.size()}; }

}  // namespace psvrecore::io
