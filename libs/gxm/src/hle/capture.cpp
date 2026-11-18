// Razor's GPU capture triggers (SceRazorCapture): a RenderDoc capture of the frame asked for, when RenderDoc is in
// the process (render.cpp, request_gpu_capture); otherwise they succeed and do nothing. A frame, as Razor counts
// them, is a call of sceGxmPadHeartbeat; the capture's file name is the game's, shown as the capture's title (RenderDoc
// keeps its captures where its UI says). Salvage mode (a capture after a GPU crash) and Razor's memory options are
// accepted (docs/PROFILING.md).

#include <atomic>

#include "psvrecore/log.hpp"
#include "state.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::gxm::hle {

namespace {

constexpr u32 kSceRazorCapture = 0x7B2A3F18;

struct Trigger {
    nx::mutex lock;
    u64 heartbeats = 0;
    u64 at = 0;  // (the heartbeat to capture after; 0: none)
    nx::string file;
};
Trigger& trigger() {
    static Trigger t;
    return t;
}

nx::string file_name(Ptr<const char> name) {
    return name ? runtime::runtime().memory().string(name.address, 256) : nx::string("(no file name)");
}

void set_trigger(u32 frame, Ptr<const char> name) {
    Trigger& t = trigger();
    nx::scoped_lock<nx::mutex> held(t.lock);
    t.at = frame;
    t.file = file_name(name);
    nx::logi(logs::gpu, "Razor: a GPU capture asked for at frame {} ('{}')", frame, t.file);
}
void sceRazorGpuCaptureSetTrigger(u32 frame, Ptr<const char> name) { set_trigger(frame, name); }
void sceRazorGpuCaptureSetTriggerNextFrame(Ptr<const char> name) {
    request_gpu_capture(nx::format("Razor trigger '{}'", file_name(name)));
}
u32 sceRazorGpuCaptureIsInProgress() { return gpu_capture_in_progress() ? 1 : 0; }
// (a capture after a GPU crash; what Razor's captures hold: RenderDoc's are its own)
void sceRazorGpuCaptureEnableSalvage(Ptr<const char> /*name*/) {}
i32 razor_capture_accepted() { return 0; }

const runtime::hle::Function kFunctions[] = {
    HLE_FUNCTION(0x472492DA, sceRazorGpuCaptureSetTrigger),
    HLE_FUNCTION(0x9DAA7E9C, sceRazorGpuCaptureSetTriggerNextFrame),
    HLE_FUNCTION(0xD2358BFB, sceRazorGpuCaptureIsInProgress),
    HLE_FUNCTION(0xDFBDCC4C, sceRazorGpuCaptureEnableSalvage),
    // (the deprecated names)
    runtime::hle::Function{0xE916B538, "sceRazorCaptureSetTrigger", &runtime::hle::entry<&sceRazorGpuCaptureSetTrigger>},
    runtime::hle::Function{0x3D4B7E68, "sceRazorCaptureSetTriggerNextFrame",
                           &runtime::hle::entry<&sceRazorGpuCaptureSetTriggerNextFrame>},
    runtime::hle::Function{0x911E0AA0, "sceRazorCaptureIsInProgress", &runtime::hle::entry<&sceRazorGpuCaptureIsInProgress>},
    runtime::hle::Function{0x3F0913CF, "sceRazorGpuCaptureStartSalvageMode", &runtime::hle::entry<&razor_capture_accepted>},
    runtime::hle::Function{0x6A09822D, "sceRazorGpuCaptureSetCaptureAllMemory", &runtime::hle::entry<&razor_capture_accepted>},
    runtime::hle::Function{0x8EE74EB7, "sceRazorGpuCaptureSetCaptureBeforeKick", &runtime::hle::entry<&razor_capture_accepted>},
};

}  // namespace

void heartbeat() {
    Trigger& t = trigger();
    nx::string file;
    {
        nx::scoped_lock<nx::mutex> held(t.lock);
        ++t.heartbeats;
        if (!t.at || t.heartbeats + 1 < t.at) return;  // (the frame asked for is the one after this heartbeat)
        t.at = 0;
        file = t.file;
    }
    request_gpu_capture(nx::format("Razor trigger '{}'", file));
}

void register_capture() {
    {
        Trigger& t = trigger();
        nx::scoped_lock<nx::mutex> held(t.lock);
        t.heartbeats = t.at = 0;
    }
    runtime::hle::register_library({"SceRazorCapture", kSceRazorCapture, kFunctions});
}

}  // namespace psvrecore::gxm::hle
