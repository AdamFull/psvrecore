// The sample runner: boots a recompiled SDK sample on the runtime with the GXM layer (as a game is booted: its
// eboot.bin and sce_module/libc.suprx, libfios2.suprx in an application folder mounted as app0:, a writable folder
// as ux0:). It writes each frame the sample shows to <output folder>/frame_NNN.png and what each audio port plays to
// <output folder>/audio_port<N>.wav, and ends the sample after <frames> frames and <audio seconds> seconds of audio
// on a port (0: not waited for), or when it exits.
//
//     sample_<name> <application folder> <data folder> <output folder> <frames> [timeout seconds] [audio seconds]
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
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/sync.h"
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

// What the audio ports play, kept per port and written as WAV files at the end.
class AudioCapture : public runtime::AudioSink {
public:
    struct Track {
        u32 rate = 0, channels = 0;
        nx::vector<i16> samples;
    };
    nx::mutex lock;
    nx::hash_map<u32, Track> tracks;
    std::atomic<u64> longest_ms{0};

    void play(const runtime::AudioBlock& b) override {
        nx::scoped_lock<nx::mutex> held(lock);
        Track& t = tracks[b.port];
        t.rate = b.rate, t.channels = b.channels;
        t.samples.insert(t.samples.end(), b.samples.begin(), b.samples.end());
        const u64 ms = t.samples.size() / t.channels * 1000 / t.rate;
        if (ms > longest_ms.load()) longest_ms.store(ms);
    }

    void write(const nx::string& folder) {
        nx::scoped_lock<nx::mutex> held(lock);
        for (const auto& [port, t] : tracks) {
            const nx::string path = nx::format("{}/audio_port{}.wav", folder, port);
            std::FILE* f = std::fopen(path.c_str(), "wb");
            if (!f) continue;
            const u32 data = static_cast<u32>(t.samples.size() * 2);
            auto u32le = [&](u32 v) { std::fwrite(&v, 4, 1, f); };
            auto u16le = [&](u16 v) { std::fwrite(&v, 2, 1, f); };
            std::fwrite("RIFF", 1, 4, f);
            u32le(36 + data);
            std::fwrite("WAVEfmt ", 1, 8, f);
            u32le(16);
            u16le(1);  // (PCM)
            u16le(static_cast<u16>(t.channels));
            u32le(t.rate);
            u32le(t.rate * t.channels * 2);
            u16le(static_cast<u16>(t.channels * 2));
            u16le(16);
            std::fwrite("data", 1, 4, f);
            u32le(data);
            std::fwrite(t.samples.data(), 2, t.samples.size(), f);
            std::fclose(f);
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc < 5) {
        std::fprintf(stderr,
                     "usage: %s <application folder> <data folder> <output folder> <frames> [timeout seconds] "
                     "[audio seconds]\n",
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
    const u64 audio_ms = argc > 6 ? std::strtoull(argv[6], nullptr, 10) * 1000 : 0;

    FrameWriter frames;
    frames.folder = argv[3];
    frames.wanted = static_cast<u32>(std::strtoul(argv[4], nullptr, 10));
    AudioCapture audio;
    gxm::Options gxm_options;
    gxm_options.display = &frames;
    gxm_options.device.validation = true;

    Runtime::Config config;
    config.files.mounts.push_back({"app0", argv[1], false});
    config.files.mounts.push_back({"ux0", argv[2], true});
    config.audio = &audio;
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

    // Until the frames are shown and the audio played, the sample exits, or the time is up.
    const u64 give_up = nx::os::ticks_ns() + timeout_s * 1'000'000'000ull;
    auto done = [&] { return frames.shown.load() >= frames.wanted && audio.longest_ms.load() >= audio_ms; };
    while (!rt.exited() && !done() && nx::os::ticks_ns() < give_up)
        nx::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::fflush(stdout);
    audio.write(frames.folder);
    const u32 shown = frames.shown.load();
    const nx::string exit_text = rt.exited() ? nx::format(", exit code {}", rt.exit_code()) : nx::string();
    std::fprintf(stderr, "%u frame(s) shown, %llu ms of audio%s\n", shown,
                 static_cast<unsigned long long>(audio.longest_ms.load()), exit_text.c_str());
    std::fflush(stderr);
    // (guest threads are not waited for: the process ends here)
    std::_Exit(done() || (rt.exited() && rt.exit_code() == 0) ? 0 : 3);
}
