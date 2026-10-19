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
    // Called on a game thread: the one setting the frame buffer for at once, else, once the next vertical blank has
    // come, the next one to wait for a blank or set a frame buffer. The frame's memory is readable until the call
    // returns.
    virtual void show(const Frame& frame) = 0;
};

struct Options {
    DisplaySink* display = nullptr;
    vulkan::DeviceOptions device;  // (the device is made at sceGxmInitialize)
    // A device to render with instead (the window's: one device a process, as volk's entry points are the last
    // device's); it must outlive the runtime.
    vulkan::Device* shared_device = nullptr;
    // Where the shader cache is kept (the SPIR-V translated, the pipelines made, the driver's pipeline cache), from
    // one run to the next: empty, nowhere.
    nx::string cache_folder;
};

// Registers SceGxm and SceDisplay. The options (and the sink) must outlive the runtime.
void register_libraries(const Options& options);

// After register_libraries, before the game starts: everything the shader cache holds made (the shaders' modules,
// the pipelines), so what an earlier run drew with is not translated or compiled while this one plays.
struct WarmUp {
    u32 shaders = 0, pipelines = 0;
    f64 ms = 0;
};
WarmUp warm_up();
// The driver's pipeline cache saved, if pipelines were made since it last was (cheap otherwise).
void save_caches();
// What was made while the game ran because the cache did not have it.
struct PlayStats {
    u32 translated = 0, pipelines = 0;
};
PlayStats play_stats();

}  // namespace psvrecore::gxm
