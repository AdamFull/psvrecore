#pragma once

#include "core/foundation/core/foundation.h"

namespace psvrecore::io {

// Writes 8-bit RGBA pixels (rows from the top, no padding) as a PNG file.
bool write_png(const char* path, u32 width, u32 height, const u8* rgba);

}  // namespace psvrecore::io
