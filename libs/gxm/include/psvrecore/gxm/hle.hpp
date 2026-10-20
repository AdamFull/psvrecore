#pragma once

// The GXM and display system libraries (SceGxm, SceDisplay) on the runtime: what a game calls to render and show
// frames. Registered with the runtime's HLE registry before a game boots; they render with the Vulkan backend and
// hand the frames the game shows to a sink (a window, or a test that writes them to files).

#include "psvrecore/vulkan/device.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::vulkan {
struct Image;
}

namespace psvrecore::gxm {

// A frame the game shows: its frame buffer in guest memory, as sceDisplaySetFrameBuf gave it.
struct Frame {
    u32 address = 0;
    u32 pitch = 0;         // in pixels
    u32 pixel_format = 0;  // SCE_DISPLAY_PIXELFORMAT_* (0: A8B8G8R8)
    u32 width = 0, height = 0;
    u64 vcount = 0;        // the vertical blank it was shown at
    // At an internal resolution above 1 (Options::render_scale): the frame as drawn, scaled_width x scaled_height,
    // tightly packed in the frame's pixel format; null when the frame was not drawn by a scene, or the CPU wrote to
    // it since. Readable until show() returns.
    const u8* scaled = nullptr;
    u32 scaled_width = 0, scaled_height = 0;
    // Instead: the frame (scaled_width x scaled_height) copied on the GPU into the image the sink's gpu_frame()
    // gave, by commands submitted to the shared device's queue before show(); in the image's layout then, for
    // sampling (VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL).
    bool gpu = false;
};

class DisplaySink {
public:
    virtual ~DisplaySink() = default;
    // Called on a game thread: the one setting the frame buffer for at once, else, once the next vertical blank has
    // come, the next one to wait for a blank or set a frame buffer. The frame's memory is readable until the call
    // returns.
    virtual void show(const Frame& frame) = 0;
    // At an internal resolution above 1, with the shared device: an image of it (width x height, four bytes a pixel,
    // sampled and copied into) for the frame shown next, copied there on the GPU instead of read back (show() then
    // has Frame::gpu); null to take the frame's pixels instead. The renderer records into it, and keeps its
    // layout, from the thread that calls show() next.
    virtual vulkan::Image* gpu_frame(u32 width, u32 height) {
        (void)width, (void)height;
        return nullptr;
    }
};

struct Options {
    DisplaySink* display = nullptr;
    vulkan::DeviceOptions device;  // (the device is made at sceGxmInitialize)
    // A device to render with instead (the window's: one device a process, as volk's entry points are the last
    // device's); it must outlive the runtime.
    vulkan::Device* shared_device = nullptr;
    // The internal resolution: colour surfaces drawn at this many times their size (1 to 4; 1 where the device
    // cannot scale a format), blitted down into guest memory when each scene ends. The frames shown come at the
    // higher resolution (Frame::scaled); what the game reads back, and samples as textures, is the surface's size.
    u32 render_scale = 1;
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
