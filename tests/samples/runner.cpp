// The sample runner: boots a recompiled SDK GXM sample on the runtime with the GXM layer (as a game is booted:
// its eboot.bin and sce_module/libc.suprx, libfios2.suprx in an application folder mounted as app0:, a writable
// folder as ux0:), writes each frame it shows to <frames folder>/frame_NNN.png and ends it after <frames> frames.
//
//     sample_<name> <application folder> <data folder> <frames folder> <frames> [timeout seconds]
//
// Built by tests/samples/CMakeLists.txt with the sample's and libc's generated code.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "png.hpp"
#include "psvrecore/gxm/hle.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/runtime/runtime.hpp"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/thread.h"

using namespace psvrecore;
using namespace psvrecore::runtime;

extern "C" {
extern const PsvModuleCode PSVRECORE_SAMPLE_MODULE;
extern const PsvModuleCode libc_module;
extern const PsvModuleCode libfios2_module;
}

namespace {

class FrameWriter : public gxm::DisplaySink {
public:
    nx::string folder;
    u32 wanted = 0;
    std::atomic<u32> shown{0};

    void show(const gxm::Frame& f) override {
        const u32 n = shown.load();
        if (n >= wanted) return;
        const u8* base = runtime::runtime().memory().host(f.address);
        nx::vector<u8> rgba(usize{f.width} * f.height * 4);
        for (u32 y = 0; y < f.height; ++y) {
            std::memcpy(rgba.data() + usize{y} * f.width * 4, base + usize{y} * f.pitch * 4, usize{f.width} * 4);
            for (u32 x = 0; x < f.width; ++x) rgba[(usize{y} * f.width + x) * 4 + 3] = 255;  // (the display ignores alpha)
        }
        const nx::string path = nx::format("{}/frame_{:03}.png", folder, n);
        if (!write_png(path.c_str(), f.width, f.height, rgba.data()))
            std::fprintf(stderr, "could not write %s\n", path.c_str());
        shown.store(n + 1);
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr, "usage: %s <application folder> <data folder> <frames folder> <frames> [timeout seconds]\n",
                     argv[0]);
        return 2;
    }
    // (PSVRECORE_LOG=debug|info|warn|error: how much the runtime logs, to stderr; warn by default)
    nx::log::Config log_config;
    log_config.level = nx::log::Level::Warn;
    if (auto level = nx::os::environment_variable("PSVRECORE_LOG")) {
        if (*level == "debug") log_config.level = nx::log::Level::Debug;
        else if (*level == "info") log_config.level = nx::log::Level::Info;
        else if (*level == "error") log_config.level = nx::log::Level::Error;
    }
    nx::log::init(log_config);
    const u64 timeout_s = argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 120;

    FrameWriter frames;
    frames.folder = argv[3];
    frames.wanted = static_cast<u32>(std::strtoul(argv[4], nullptr, 10));
    gxm::Options gxm_options;
    gxm_options.display = &frames;
    gxm_options.device.validation = true;

    Runtime::Config config;
    config.files.mounts.push_back({"app0", argv[1], false});
    config.files.mounts.push_back({"ux0", argv[2], true});
    Runtime rt(config);
    if (!rt.ok()) return 2;
    gxm::register_libraries(gxm_options);
    rt.loader().add_code(PSVRECORE_SAMPLE_MODULE);
    rt.loader().add_code(libc_module);
    rt.loader().add_code(libfios2_module);

    Loader::BootOptions boot;
    boot.main = "app0:eboot.bin";
    boot.modules = {"app0:sce_module/libc.suprx", "app0:sce_module/libfios2.suprx"};
    boot.args = {"app0:eboot.bin"};
    auto main_thread = rt.loader().boot(boot);
    if (!main_thread) {
        std::fprintf(stderr, "boot failed: %s\n", main_thread.error().message.c_str());
        return 2;
    }

    // Until the frames are shown, the sample exits, or the time is up.
    const u64 give_up = nx::os::ticks_ns() + timeout_s * 1'000'000'000ull;
    while (!rt.exited() && frames.shown.load() < frames.wanted && nx::os::ticks_ns() < give_up)
        nx::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::fflush(stdout);
    const u32 shown = frames.shown.load();
    std::fprintf(stderr, "%u frame(s) shown%s\n", shown, rt.exited() ? nx::format(", exit code {}", rt.exit_code()).c_str() : "");
    std::fflush(stderr);
    // (guest threads are not waited for: the process ends here)
    std::_Exit(shown >= frames.wanted ? 0 : 3);
}
