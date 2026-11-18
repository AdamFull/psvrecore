#pragma once

// RenderDoc's in-application API (third_party/renderdoc/renderdoc_app.h, MIT), when RenderDoc is in the process: the
// game started from RenderDoc's UI or renderdoccmd, or RenderDoc injected into it. Nothing is loaded otherwise; every
// call below then does nothing (docs/PROFILING.md).

#include "volk.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/strings/utf8_string.h"

namespace psvrecore::vulkan::renderdoc {

// RenderDoc is in the process (looked for once, at the first call). Its own capture keys are turned off then: the
// frontend's key captures a frame of the game's (a guest frame, its scenes), which RenderDoc's next-present capture
// does not bound.
bool loaded();
// A capture of what the instance's device does from start() to end(); end() is true if a capture was written.
// `title`: shown with the capture in RenderDoc's list (empty: none).
void start(VkInstance instance, nx::string_view title = {});
bool end(VkInstance instance);
bool capturing();

}  // namespace psvrecore::vulkan::renderdoc
