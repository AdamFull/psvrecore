#pragma once

// The GXM and display system libraries (SceGxm, SceDisplay) on the runtime: what a game calls to render and show
// frames. Registered with the runtime's HLE registry before a game boots; they render with the Vulkan backend and
// hand the frames the game shows to a sink (a window, or a test that writes them to files).

#include "psvrecore/vulkan/device.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::gxm {

// A frame the game shows: its frame buffer in guest memory, as sceDisplaySetFrameBuf gave it.
struct Frame {
    u32 address = 0;
    u32 pitch = 0;         // in pixels
    u32 pixel_format = 0;  // SCE_DISPLAY_PIXELFORMAT_* (0: A8B8G8R8)
    u32 width = 0, height = 0;
    u64 vcount = 0;        // the vertical blank it was shown at
};

class DisplaySink {
public:
    virtual ~DisplaySink() = default;
    // Called on the thread that set the frame buffer; the frame's memory is readable until the call returns.
    virtual void show(const Frame& frame) = 0;
};

struct Options {
    DisplaySink* display = nullptr;
    vulkan::DeviceOptions device;  // (the device is made at sceGxmInitialize)
    f64 refresh_hz = 59.94005994;  // the display's vertical blanks
};

// Registers SceGxm and SceDisplay. The options (and the sink) must outlive the runtime.
void register_libraries(const Options& options);

}  // namespace psvrecore::gxm
