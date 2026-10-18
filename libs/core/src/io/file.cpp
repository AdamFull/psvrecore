#include "psvrecore/io/file.hpp"

#include "core/foundation/strings/format.h"

namespace psvrecore::io {

namespace {

Error file_error(nx::string_view what, nx::fs::path_view path, const nx::fs::file_error& e) {
    return Error{nx::format("cannot {} '{}': {}", what, path.native(), nx::fs::to_string(e.kind))};
}

}  // namespace

Result<nx::blob<u8>> read_file(nx::fs::path_view path) {
    auto data = nx::fs::file_read(path);
    if (!data) return nx::err(file_error("read", path, data.error()));
    return std::move(*data);
}

Result<nx::string> read_text_file(nx::fs::path_view path) {
    auto text = nx::fs::file_read_text(path);
    if (!text) return nx::err(file_error("read", path, text.error()));
    return std::move(*text);
}

Result<void> write_file(nx::fs::path_view path, std::span<const u8> data) {
    if (const auto written = nx::fs::file_write_atomic(path, data); !written)
        return nx::err(file_error("write", path, written.error()));
    return {};
}

Result<void> write_text_file(nx::fs::path_view path, nx::string_view text) {
    if (const auto written = nx::fs::file_write_text_atomic(path, text); !written)
        return nx::err(file_error("write", path, written.error()));
    return {};
}

}  // namespace psvrecore::io
