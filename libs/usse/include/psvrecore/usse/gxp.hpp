#pragma once

// GXP files: compiled shader programs (a vertex or fragment program: a primary program run per vertex or fragment,
// a secondary one run per draw, and their parameter tables). Only what the decoder needs so far: the header and the
// two programs' words.

#include <span>

#include "psvrecore/error.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::usse {

enum class ShaderType : u8 { Vertex, Fragment };

struct Gxp {
    ShaderType type = ShaderType::Vertex;
    u32 size = 0;  // the file's own size field
    nx::vector<u64> primary;
    nx::vector<u64> secondary;
};

// Reads a GXP held in memory (the words are copied out).
Result<Gxp> parse_gxp(std::span<const u8> data);

}  // namespace psvrecore::usse
