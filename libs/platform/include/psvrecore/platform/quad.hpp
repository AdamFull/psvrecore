#pragma once

#include "core/foundation/core/foundation.h"

namespace psvrecore::platform {

// A textured rectangle the window draws: where (in the window's pixels), what part of its texture (0 to 1), and a
// tint its texels are multiplied by.
struct Quad {
    f32 x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    f32 u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    f32 r = 1, g = 1, b = 1, a = 1;
};

}  // namespace psvrecore::platform
