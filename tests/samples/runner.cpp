// The sample runner: boots a recompiled SDK sample on the runtime with the GXM layer (as a game is booted: its
// eboot.bin and sce_module/libc.suprx, libfios2.suprx in an application folder mounted as app0:, a writable folder
// as ux0:, and its savedata folder as savedata0:, read-only to sceIo as on the console). It writes each frame the
// sample shows to <output folder>/frame_NNN.png and what each audio port plays to <output folder>/audio_port<N>.wav,
// and ends the sample after <frames> frames and <audio seconds> seconds of audio on a port (0: not waited for), or
// when it exits.
//
//     sample_<name> <application folder> <data folder> <output folder> <frames> [timeout seconds] [audio seconds]
//
// With PSVRECORE_WINDOW=1 the sample is also shown in a window, its sound played on the audio device and the host's
// controls given to it (psvrecore::platform); it then runs until the window is closed or the time is up, and
// PSVRECORE_INPUT may script controls: items `<ms> <what> <args>` separated by ';', the time from the boot:
//
//     key <SDL key name> down|up         pad <SDL gamepad button> down|up      axis <SDL gamepad axis> <value>
//     touch <x> <y> down|move|up (the screen's 960 x 544)                      shot <name> (the next frame to <name>.png)
//
// Built by tests/samples/CMakeLists.txt with the sample's and libc's generated code.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>

#include "png.hpp"
#include "psvrecore/gxm/hle.hpp"
#include "psvrecore/platform/audio.hpp"
#include "psvrecore/platform/window.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/runtime/runtime.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/platform/filesystem.h"
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

void write_frame(const gxm::Frame& f, const nx::string& path) {
    const u8* base = runtime::runtime().memory().host(f.address);
    nx::vector<u8> rgba(usize{f.width} * f.height * 4);
    for (u32 y = 0; y < f.height; ++y) {
        std::memcpy(rgba.data() + usize{y} * f.width * 4, base + usize{y} * f.pitch * 4, usize{f.width} * 4);
        for (u32 x = 0; x < f.width; ++x) rgba[(usize{y} * f.width + x) * 4 + 3] = 255;  // (the display ignores alpha)
    }
    if (!write_png(path.c_str(), f.width, f.height, rgba.data())) std::fprintf(stderr, "could not write %s\n", path.c_str());
}

class FrameWriter : public gxm::DisplaySink {
public:
    nx::string folder;
    u32 wanted = 0;
    std::atomic<u32> shown{0};
    gxm::DisplaySink* also = nullptr;  // (the window)
    nx::mutex shot_lock;
    nx::vector<nx::string> shots;  // (names of the next frames to write)

    void show(const gxm::Frame& f) override {
        if (also) also->show(f);
        {
            nx::scoped_lock<nx::mutex> held(shot_lock);
            if (!shots.empty()) {
                write_frame(f, nx::format("{}/{}.png", folder, shots.front()));
                shots.erase(shots.begin());
            }
        }
        const u32 n = shown.load();
        if (n >= wanted) return;
        write_frame(f, nx::format("{}/frame_{:03}.png", folder, n));
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
    runtime::AudioSink* also = nullptr;  // (the audio device)

    void play(const runtime::AudioBlock& b) override {
        if (also) also->play(b);
        nx::scoped_lock<nx::mutex> held(lock);
        Track& t = tracks[b.port];
        t.rate = b.rate, t.channels = b.channels;
        t.samples.insert(t.samples.end(), b.samples.begin(), b.samples.end());
        const u64 ms = t.samples.size() / t.channels * 1000 / t.rate;
        if (ms > longest_ms.load()) longest_ms.store(ms);
    }

    void port_released(u32 port) override {
        if (also) also->port_released(port);
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

// A scripted control (PSVRECORE_INPUT), as an event the window takes like the host's.
struct Scripted {
    u64 at_ms = 0;
    nx::string what, a, b, c;
};

nx::vector<Scripted> parse_script(const char* text) {
    nx::vector<Scripted> items;
    nx::string item;
    auto flush = [&] {
        char what[32] = {}, a[64] = {}, b[64] = {}, c[64] = {};
        unsigned long long ms = 0;
        if (std::sscanf(item.c_str(), " %llu %31s %63s %63s %63s", &ms, what, a, b, c) >= 2) {
            Scripted s;
            s.at_ms = ms, s.what = what, s.a = a, s.b = b, s.c = c;
            items.push_back(s);
        }
        item.clear();
    };
    for (const char* p = text; *p; ++p) {
        if (*p == ';') flush();
        else item.push_back(*p);
    }
    flush();
    return items;
}

void play_scripted(const Scripted& s, platform::Window& window, FrameWriter& frames) {
    SDL_Event e{};
    if (s.what == "key") {
        e.type = s.b == "down" ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        e.key.key = SDL_GetKeyFromName(s.a.c_str());
        e.key.down = s.b == "down";
    } else if (s.what == "pad") {
        e.type = s.b == "down" ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
        e.gbutton.which = 0x7FFFFFFF;  // (a gamepad of the script's own)
        e.gbutton.button = static_cast<u8>(SDL_GetGamepadButtonFromString(s.a.c_str()));
        e.gbutton.down = s.b == "down";
    } else if (s.what == "axis") {
        e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
        e.gaxis.which = 0x7FFFFFFF;
        e.gaxis.axis = static_cast<u8>(SDL_GetGamepadAxisFromString(s.a.c_str()));
        e.gaxis.value = static_cast<i16>(std::strtol(s.b.c_str(), nullptr, 10));
    } else if (s.what == "touch") {
        // (the screen's point in the window: where the picture is drawn)
        int w = 0, h = 0;
        SDL_GetWindowSize(window.sdl(), &w, &h);
        const f32 scale =
            std::min(static_cast<f32>(w) / platform::kScreenWidth, static_cast<f32>(h) / platform::kScreenHeight);
        const f32 x = (static_cast<f32>(w) - platform::kScreenWidth * scale) / 2 + std::strtof(s.a.c_str(), nullptr) * scale;
        const f32 y = (static_cast<f32>(h) - platform::kScreenHeight * scale) / 2 + std::strtof(s.b.c_str(), nullptr) * scale;
        if (s.c == "move") {
            e.type = SDL_EVENT_MOUSE_MOTION;
            e.motion.which = 1;
            e.motion.x = x, e.motion.y = y;
        } else {
            e.type = s.c == "up" ? SDL_EVENT_MOUSE_BUTTON_UP : SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.which = 1;
            e.button.button = SDL_BUTTON_LEFT;
            e.button.down = s.c != "up";
            e.button.x = x, e.button.y = y;
        }
    } else if (s.what == "shot") {
        nx::scoped_lock<nx::mutex> held(frames.shot_lock);
        frames.shots.push_back(s.a);
        return;
    } else {
        std::fprintf(stderr, "PSVRECORE_INPUT: what is '%s'?\n", s.what.c_str());
        return;
    }
    e.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&e);
}

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

    // The window, the audio device and the controls (PSVRECORE_WINDOW=1).
    std::unique_ptr<platform::Window> window;
    std::unique_ptr<platform::AudioDevice> speaker;
    nx::vector<Scripted> script;
    if (auto w = nx::os::environment_variable("PSVRECORE_WINDOW"); w && *w == "1") {
        platform::Window::Options options;
        options.title = "psvrecore sample";
        options.validation = gxm_options.device.validation;
        if (auto title = nx::os::environment_variable("PSVRECORE_TITLE")) options.title = *title;
        auto opened = platform::Window::open(options);
        if (!opened) {
            std::fprintf(stderr, "no window: %s\n", opened.error().message.c_str());
            return 2;
        }
        window = std::move(*opened);
        frames.also = window.get();
        gxm_options.shared_device = window->device();
        if (auto device = platform::AudioDevice::open()) {
            speaker = std::move(*device);
            audio.also = speaker.get();
        } else {
            std::fprintf(stderr, "no sound: %s\n", device.error().message.c_str());
        }
        if (auto text = nx::os::environment_variable("PSVRECORE_INPUT")) script = parse_script(text->c_str());
    }

    Runtime::Config config;
    config.files.mounts.push_back({"app0", argv[1], false});
    config.files.mounts.push_back({"ux0", argv[2], true});
    const nx::string savedata = nx::string(argv[2]) + "/savedata";
    (void)nx::fs::create_directories(nx::fs::path_view(savedata));
    config.files.mounts.push_back({"savedata0", savedata, false});
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

    // Until the frames are shown and the audio played, the sample exits, or the time is up (with a window: until it
    // is closed, the sample exits, or the time is up).
    const u64 booted = nx::os::ticks_ns();
    const u64 give_up = booted + timeout_s * 1'000'000'000ull;
    auto done = [&] { return frames.shown.load() >= frames.wanted && audio.longest_ms.load() >= audio_ms; };
    usize next_scripted = 0;
    bool closed = false;
    while (!rt.exited() && nx::os::ticks_ns() < give_up) {
        if (!window) {
            if (done()) break;
            nx::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const u64 ms = (nx::os::ticks_ns() - booted) / 1'000'000;
        while (next_scripted < script.size() && script[next_scripted].at_ms <= ms)
            play_scripted(script[next_scripted++], *window, frames);
        if (!window->pump(&rt, 5)) {
            closed = true;
            break;
        }
    }
    std::fflush(stdout);
    audio.write(frames.folder);
    const u32 shown = frames.shown.load();
    const nx::string exit_text = rt.exited() ? nx::format(", exit code {}", rt.exit_code()) : nx::string();
    std::fprintf(stderr, "%u frame(s) shown, %llu ms of audio%s\n", shown,
                 static_cast<unsigned long long>(audio.longest_ms.load()), exit_text.c_str());
    if (window)
        std::fprintf(stderr, "window: %llu frame(s) presented%s\n", static_cast<unsigned long long>(window->presented()),
                     closed ? ", closed" : "");
    std::fflush(stderr);
    // (guest threads are not waited for: the process ends here)
    std::_Exit(done() || (rt.exited() && rt.exit_code() == 0) ? 0 : 3);
}
