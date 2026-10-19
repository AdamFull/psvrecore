#include "psvrecore/frontend/frontend.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "psvrecore/io/png.hpp"
#include "psvrecore/frontend/script.hpp"
#include "psvrecore/gxm/hle.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/platform/audio.hpp"
#include "psvrecore/platform/window.hpp"
#include "psvrecore/runtime/runtime.hpp"
#include "psvrecore/vulkan/device.hpp"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/serialization/arguments.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/sync.h"

namespace psvrecore::frontend {

namespace {

// The display: the window, and the frames the script asks for saved.
class Display : public gxm::DisplaySink {
public:
    platform::Window* window = nullptr;
    nx::string out;
    nx::mutex lock;
    nx::vector<nx::string> shots;

    void show(const gxm::Frame& f) override {
        if (window) window->show(f);
        nx::string name;
        {
            nx::scoped_lock<nx::mutex> held(lock);
            if (shots.empty()) return;
            name = shots.front();
            shots.erase(shots.begin());
        }
        const u8* base = runtime::runtime().memory().host(f.address);
        nx::vector<u8> rgba(usize{f.width} * f.height * 4);
        for (u32 y = 0; y < f.height; ++y) {
            const u8* row = base + usize{y} * f.pitch * 4;
            u8* dst = rgba.data() + usize{y} * f.width * 4;
            for (u32 x = 0; x < f.width; ++x) {
                u32 v;
                std::memcpy(&v, row + x * 4, 4);
                if (f.pixel_format == 0x50000000u) {  // (A2B10G10R10)
                    dst[x * 4 + 0] = static_cast<u8>(v >> 2), dst[x * 4 + 1] = static_cast<u8>(v >> 12);
                    dst[x * 4 + 2] = static_cast<u8>(v >> 22);
                } else {
                    std::memcpy(dst + x * 4, &v, 3);
                }
                dst[x * 4 + 3] = 255;  // (the display ignores alpha)
            }
        }
        const nx::string path = nx::format("{}/{}.png", out, name);
        if (!io::write_png(path.c_str(), f.width, f.height, rgba.data()))
            nx::loge(logs::frontend, "could not write {}", path);
        else
            nx::logi(logs::frontend, "shot: {}", path);
    }
};

nx::log::Level log_level(nx::string_view name) {
    if (name == "debug") return nx::log::Level::Debug;
    if (name == "info") return nx::log::Level::Info;
    if (name == "error") return nx::log::Level::Error;
    return nx::log::Level::Warn;
}

nx::string join(nx::string_view folder, nx::string_view name) {
    nx::string path(folder);
    if (!path.empty() && path.back() != '/' && path.back() != '\\') path += "/";
    path += nx::string(name);
    return path;
}

// The user's data folder for the program (made if missing; empty if there is none).
nx::string user_folder(const Game& game) {
    const nx::string app(game.name);
    char* pref = SDL_GetPrefPath("psvrecore", app.c_str());
    if (!pref) return {};
    nx::string path(pref);
    SDL_free(pref);
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    return path;
}

}  // namespace

Result<CommandLine> parse_command_line(const Game& game, int argc, const char* const* argv) {
    CommandLine c;
    nx::string game_folder, data_folder, scale, gpu, language, log, log_file;
    bool fullscreen = false, windowed = false, validation = false, no_vsync = false, no_audio = false;
    nx::cli::Parser parser(game.name, game.title);
    parser.root()
        .option("game", game_folder, "the game's application folder (game.folder)", {.value_name = "FOLDER"})
        .option("data", data_folder, "where ux0: and save data are kept (game.data)", {.value_name = "FOLDER"})
        .option("settings", c.settings_file, "the settings file (default: settings.ini in the user's data folder)",
                {.value_name = "FILE"})
        .option_many("set", c.assignments, "a setting, over the file's", {.value_name = "SECTION.KEY=VALUE"})
        .flag("fullscreen", fullscreen, "full screen (display.fullscreen=true)")
        .flag("windowed", windowed, "in a window (display.fullscreen=false)")
        .option("scale", scale, "the window's size in screens (display.scale)", {.value_name = "N"})
        .option("gpu", gpu, "the Vulkan device's index (display.gpu)", {.value_name = "N"})
        .flag("list-gpus", c.list_gpus, "list the Vulkan devices and end", {.terminal = true})
        .flag("validation", validation, "Vulkan's validation layer (display.validation=true)")
        .flag("no-vsync", no_vsync, "present without waiting for the display (display.vsync=false)")
        .flag("no-audio", no_audio, "no sound (audio.enabled=false)")
        .option("language", language, "the console's language (system.language)", {.value_name = "NAME"})
        .option("log", log, "how much is logged: debug, info, warn, error (log.level)", {.value_name = "LEVEL"})
        .option("log-file", log_file, "a file the log is written to as well (log.file)", {.value_name = "FILE"})
        .option("input", c.input, "scripted controls, or @file (see psvrecore/frontend/script.hpp)",
                {.value_name = "SCRIPT"})
        .option("out", c.out, "where the script's shots are saved (default: the current folder)",
                {.value_name = "FOLDER"})
        .option("timeout", c.timeout_s, "end after this many seconds", {.value_name = "SECONDS"})
        .flag("hidden", c.hidden, "no window on screen and no message boxes (tests)");
    const nx::cli::ParseResult parsed = parser.parse(argc, argv);
    if (parsed.status == nx::cli::ParseStatus::Help) {
        c.help = true;
        c.help_text = parser.help();
        return c;
    }
    if (!parsed.parsed()) return failure(nx::format("{}\n\n{}", parsed.message, parser.help()));
    if (fullscreen && windowed) return failure("--fullscreen and --windowed together");
    // (the options that stand for a setting go first, so --set has the last word)
    nx::vector<nx::string> shortcuts;
    auto add = [&](const char* key, const nx::string& value) {
        if (!value.empty()) shortcuts.push_back(nx::format("{}={}", key, value));
    };
    add("game.folder", game_folder);
    add("game.data", data_folder);
    if (fullscreen) shortcuts.push_back("display.fullscreen=true");
    if (windowed) shortcuts.push_back("display.fullscreen=false");
    add("display.scale", scale);
    add("display.gpu", gpu);
    if (validation) shortcuts.push_back("display.validation=true");
    if (no_vsync) shortcuts.push_back("display.vsync=false");
    if (no_audio) shortcuts.push_back("audio.enabled=false");
    add("system.language", language);
    add("log.level", log);
    add("log.file", log_file);
    for (nx::string& a : c.assignments) shortcuts.push_back(std::move(a));
    c.assignments = std::move(shortcuts);
    return c;
}

Result<Settings> resolve_settings(const CommandLine& command_line, const nx::string& default_file,
                                  nx::string* file_used) {
    Settings settings;
    const nx::string file = command_line.settings_file.empty() ? default_file : command_line.settings_file;
    *file_used = file;
    if (!file.empty()) {
        const nx::fs::path_view path(file);
        if (nx::fs::exists(path)) {
            auto text = nx::fs::file_read_text(path);
            if (!text) return failure(nx::format("{}: cannot be read", file));
            if (auto r = apply_file_text(&settings, text->view()); !r)
                return failure(nx::format("{}: {}", file, r.error().message));
        } else if (!command_line.settings_file.empty()) {
            return failure(nx::format("{}: no such settings file", file));
        } else if (!nx::fs::file_write_text_atomic(path, file_text(settings))) {
            // (not fatal: the defaults are used)
            nx::logw(logs::frontend, "{}: the default settings could not be written", file);
        }
    }
    for (const nx::string& a : command_line.assignments) {
        if (auto r = set(&settings, a.view()); !r) return failure(r.error().message);
    }
    return settings;
}

int run(const Game& game, int argc, char** argv) {
    auto command_line = parse_command_line(game, argc, argv);
    const nx::string title(game.title);
    auto fail = [&](const nx::string& message, bool hidden) {
        std::fprintf(stderr, "%s\n", message.c_str());
        if (!hidden) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), message.c_str(), nullptr);
        return 2;
    };
    if (!command_line) return fail(command_line.error().message, true);
    if (command_line->help) {
        std::printf("%s", command_line->help_text.c_str());
        return 0;
    }
    const CommandLine& cl = *command_line;
    if (cl.list_gpus) {
        auto gpus = vulkan::Device::list();
        if (!gpus) return fail(nx::format("Vulkan: {}", gpus.error().message), true);
        for (usize i = 0; i < gpus->size(); ++i) std::printf("%zu: %s\n", i, (*gpus)[i].c_str());
        return 0;
    }

    SDL_SetAppMetadata(title.c_str(), nullptr, nx::string(game.name).c_str());
    const nx::string user = user_folder(game);
    nx::string settings_file;
    auto settings = resolve_settings(cl, user.empty() ? nx::string() : join(user, "settings.ini"), &settings_file);
    if (!settings) return fail(settings.error().message, cl.hidden);
    const Settings& s = *settings;

    nx::log::Config log_config;
    log_config.level = log_level(s.log_level.view());
    if (!s.log_file.empty()) log_config.file_path = s.log_file.c_str();
    nx::log::init(log_config);
    nx::logi(logs::frontend, "{}: settings from {}", game.name, settings_file.empty() ? "defaults" : settings_file.c_str());

    // The game's folder and the data folder.
    if (s.game_folder.empty())
        return fail(nx::format("No game folder. Give it with --game <folder>, or as [game] folder in {}.",
                               settings_file.empty() ? "the settings" : settings_file.c_str()),
                    cl.hidden);
    const nx::string eboot = join(s.game_folder, "eboot.bin");
    if (!nx::fs::exists(nx::fs::path_view(eboot)))
        return fail(nx::format("{} has no eboot.bin: it is not a game's application folder.", s.game_folder), cl.hidden);
    if (game.check) {
        if (auto r = game.check(s.game_folder); !r) return fail(r.error().message, cl.hidden);
    }
    const nx::string data = !s.data_folder.empty() ? s.data_folder : user.empty() ? nx::string("data") : join(user, "data");
    const nx::string save = join(data, nx::format("user/00/savedata/{}", game.title_id));
    const nx::string safe = join(data, "user/00/safemem");
    for (const nx::string& folder : {save, safe}) {
        if (!nx::fs::create_directories(nx::fs::path_view(folder)))
            return fail(nx::format("{} cannot be made", folder), cl.hidden);
    }
    nx::vector<ScriptItem> script_items;
    if (!cl.input.empty()) {
        nx::string text = cl.input;
        if (text.front() == '@') {
            auto read = nx::fs::file_read_text(nx::fs::path_view(text.c_str() + 1));
            if (!read) return fail(nx::format("{}: cannot be read", text.c_str() + 1), cl.hidden);
            text = std::move(*read);
        }
        auto items = parse_script(text.view());
        if (!items) return fail(items.error().message, cl.hidden);
        script_items = std::move(*items);
    }

    // The window and the audio device.
    platform::Window::Options window_options;
    window_options.title = title;
    window_options.scale = s.scale;
    window_options.fullscreen = s.fullscreen;
    window_options.vsync = s.vsync;
    window_options.smooth = s.smooth;
    window_options.gpu = s.gpu;
    window_options.validation = s.validation;
    window_options.hidden = cl.hidden;
    auto window = platform::Window::open(window_options);
    if (!window) return fail(window.error().message, cl.hidden);
    std::unique_ptr<platform::AudioDevice> speaker;
    if (s.audio) {
        platform::AudioDevice::Options audio_options;
        audio_options.max_latency_ms = s.audio_latency_ms;
        if (auto device = platform::AudioDevice::open(audio_options)) speaker = std::move(*device);
        else nx::logw(logs::frontend, "no sound: {}", device.error().message);
    }
    Display display;
    display.window = window->get();
    display.out = cl.out.empty() ? nx::string(".") : cl.out;

    // The runtime, the GXM layer, the game's code.
    runtime::Runtime::Config config;
    config.files.mounts.push_back({"app0", s.game_folder, false});
    config.files.mounts.push_back({"ux0", data, true});
    config.files.mounts.push_back({"savedata0", save, false});  // (read-only to sceIo; save data is written by SceAppUtil)
    config.audio = speaker.get();
    config.system.language = s.language;
    config.system.user_name = s.user_name;
    config.system.date_format = s.date_format;
    config.system.time_format = s.time_format;
    config.system.time_zone_minutes = s.time_zone_auto ? host_time_zone_minutes() : s.time_zone_minutes;
    config.system.summer_time = s.time_zone_auto ? host_summer_time() : s.summer_time;
    config.system.safe_memory = join(safe, nx::format("{}.bin", game.title_id));
    gxm::Options gxm_options;
    gxm_options.display = &display;
    gxm_options.device.validation = s.validation;
    gxm_options.shared_device = (*window)->device();
    runtime::Runtime rt(config);
    if (!rt.ok()) return fail("The runtime could not start (see the log).", cl.hidden);
    gxm::register_libraries(gxm_options);
    for (const PsvModuleCode* code : game.code) rt.loader().add_code(*code);
    runtime::Loader::BootOptions boot;
    boot.main = nx::string(game.main);
    for (const char* m : game.modules) boot.modules.push_back(m);
    boot.args = {boot.main};
    auto main_thread = rt.loader().boot(boot);
    if (!main_thread) return fail(nx::format("The game could not start: {}", main_thread.error().message), cl.hidden);
    nx::logi(logs::frontend, "{} started from {}; data in {}", game.title, s.game_folder, data);

    // Until the window is closed, the game exits, the script quits or the time is up.
    Script script(std::move(script_items));
    const u64 booted = nx::os::ticks_ns();
    const char* ended = "the window was closed";
    while (true) {
        if (rt.exited()) {
            ended = "the game exited";
            break;
        }
        const u64 ms = (nx::os::ticks_ns() - booted) / 1'000'000;
        if (cl.timeout_s && ms >= u64{cl.timeout_s} * 1000) {
            ended = "the time was up";
            break;
        }
        const bool go_on = script.play(ms, **window, [&](const nx::string& name) {
            nx::scoped_lock<nx::mutex> held(display.lock);
            display.shots.push_back(name);
        });
        if (!go_on) {
            ended = "the script quit";
            break;
        }
        if (!(*window)->pump(&rt, 5)) break;
    }
    const i32 code = rt.exited() ? rt.exit_code() : 0;
    nx::log::flush();
    std::fprintf(stderr, "%s: %s; %llu frames presented, exit code %d\n", nx::string(game.name).c_str(), ended,
                 static_cast<unsigned long long>((*window)->presented()), code);
    std::fflush(stdout);
    std::fflush(stderr);
    // (guest threads are not waited for: the process ends here)
    std::_Exit(code);
}

}  // namespace psvrecore::frontend
