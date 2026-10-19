#pragma once

#include <span>

#include "psvrecore/gxm/reference.hpp"

namespace psvrender {

using namespace psvrecore;

struct GpuOptions {
    i32 gpu = -1;
    bool validation = false;
};

// Draws as the HLE renderer does (gxm/src/hle/render.cpp): the programs' vertex and fragment translations, the draw
// bindings and push constants, the default viewport; into an RGBA8 target that starts as `target` holds it (and a
// depth/stencil image cleared to 1). The secondary programs run in the reference interpreter (they only set up the
// secondary attributes; their translation is checked by the samples).
Result<void> render_on_gpu(const GpuOptions& options, std::span<const gxm::reference::Draw> draws,
                           gxm::reference::Image& target);

}  // namespace psvrender
