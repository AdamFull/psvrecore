#include "psvrecore/frontend/frontend.hpp"
#include "psvrecore/frontend/layout.hpp"

#include <SDL3/SDL.h>

#if defined(__ANDROID__)
#include <jni.h>
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>

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
    // The points of the screen the script's waits look at, and their colours in the last frame shown.
    nx::vector<std::pair<f32, f32>> probes;
    nx::vector<std::optional<u32>> probed;

    void show(const gxm::Frame& f) override {
        // (PSVRECORE_SHOTS_FROM_WINDOW=1: shots of what the window presented, the overlays over the picture, at the
        // window's size)
        static const bool from_window = [] {
            const char* v = std::getenv("PSVRECORE_SHOTS_FROM_WINDOW");
            return v && *v && *v != '0';
        }();
        if (from_window && window) window->keep_last(true);
        const u64 presented_before = window ? window->presented() : 0;
        if (window) window->show(f);
        if (!probes.empty()) {
            // (from the frame in guest memory, at its own size)
            const u8* base = runtime::runtime().memory().host(f.address);
            nx::scoped_lock<nx::mutex> held(lock);
            probed.resize(probes.size());
            for (usize i = 0; i < probes.size(); ++i) {
                const u32 x = static_cast<u32>(probes[i].first * f.width / platform::kScreenWidth);
                const u32 y = static_cast<u32>(probes[i].second * f.height / platform::kScreenHeight);
                if (x >= f.width || y >= f.height) continue;
                u32 v;
                std::memcpy(&v, base + (usize{y} * f.pitch + x) * 4, 4);
                probed[i] = f.pixel_format == 0x50000000u  // (A2B10G10R10; else ABGR8888)
                                ? ((v >> 2) & 0xFF) << 16 | ((v >> 12) & 0xFF) << 8 | ((v >> 22) & 0xFF)
                                : (v & 0xFF) << 16 | ((v >> 8) & 0xFF) << 8 | ((v >> 16) & 0xFF);
            }
        }
        nx::string name;
        {
            nx::scoped_lock<nx::mutex> held(lock);
            if (shots.empty()) return;
            name = shots.front();
            shots.erase(shots.begin());
        }
        if (from_window && window) {
            nx::vector<u8> pixels;
            u32 w = 0, h = 0;
            if (window->wait_presented(presented_before + 1, 500) && window->last_image(&pixels, &w, &h)) {
                const nx::string path = nx::format("{}/{}.png", out, name);
                if (io::write_png(path.c_str(), w, h, pixels.data())) nx::logi(logs::frontend, "shot (the window): {}", path);
                return;
            }
        }
        // (at the internal resolution when the frame was drawn at one)
        const u8* base = f.scaled ? f.scaled : runtime::runtime().memory().host(f.address);
        const u32 width = f.scaled ? f.scaled_width : f.width, height = f.scaled ? f.scaled_height : f.height;
        const u32 pitch = f.scaled ? f.scaled_width : f.pitch;
        nx::vector<u8> rgba(usize{width} * height * 4);
        for (u32 y = 0; y < height; ++y) {
            const u8* row = base + usize{y} * pitch * 4;
            u8* dst = rgba.data() + usize{y} * width * 4;
            for (u32 x = 0; x < width; ++x) {
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
        if (!io::write_png(path.c_str(), width, height, rgba.data()))
            nx::loge(logs::frontend, "could not write {}", path);
        else
            nx::logi(logs::frontend, "shot: {}", path);
    }
};

// The application sent to the background and brought back (Android): the game suspended, its sound paused and the
// window's surface let go, all before the system takes the window. From the activity's pause and resume (on the
// system's thread, before the surface goes), and from SDL's events as well (which reach SDL's thread only after).
struct Lifecycle {
    runtime::Runtime* rt = nullptr;
    platform::AudioDevice* audio = nullptr;
    platform::Window* window = nullptr;
    nx::mutex lock;
    bool background = false;
    std::atomic<bool> sound{true};  // (the settings' audio.enabled: no sound back from the background without it)

    void set_background(bool to) {
        nx::scoped_lock<nx::mutex> held(lock);
        if (background == to) return;
        background = to;
        if (to) {
            rt->kernel().suspend();
            if (audio) audio->set_paused(true);
            window->set_background(true);
            gxm::save_caches();  // (the system may end the process in the background)
            nx::logi(logs::frontend, "to the background: suspended");
        } else {
            window->set_background(false);
            if (audio && sound.load()) audio->set_paused(false);
            rt->kernel().resume();
            nx::logi(logs::frontend, "back from the background: resumed");
        }
    }
};

std::atomic<Lifecycle*> g_lifecycle{nullptr};
// Set when a settings screen has changed the settings file: what applies while the game runs is read again.
std::atomic<bool> g_settings_changed{false};

nx::log::Level log_level(nx::string_view name);

// The settings that apply while the game runs (SettingInfo::live), from `s`.
void apply_live(const Settings& s, Lifecycle& l) {
    platform::TouchControls& controls = l.window->input().controls();
    controls.set_mode(static_cast<platform::TouchControls::Mode>(s.on_screen));
    controls.set_opacity(s.controls_opacity);
    controls.set_scale(s.controls_scale);
    l.window->set_smooth(s.smooth);
    l.window->set_fps_overlay(s.fps_overlay);
    l.sound.store(s.audio);
    if (l.audio) l.audio->set_paused(!s.audio);
    nx::log::set_level(log_level(s.log_level.view()));
    l.window->refresh_view();
}

// The settings screen (Android: SettingsActivity, over the game, which is suspended meanwhile).
void open_settings() {
#if defined(__ANDROID__)
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return;
    jclass cls = env->GetObjectClass(activity);
    if (jmethodID open = env->GetMethodID(cls, "openSettings", "()V")) env->CallVoidMethod(activity, open);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
#endif
}

bool on_app_event(void* user, SDL_Event* e) {
    Lifecycle& l = *static_cast<Lifecycle*>(user);
    if (e->type == SDL_EVENT_WILL_ENTER_BACKGROUND) l.set_background(true);
    else if (e->type == SDL_EVENT_DID_ENTER_FOREGROUND) l.set_background(false);
    else if (e->type == SDL_EVENT_TERMINATING) nx::log::flush();
    return true;
}

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

// The user's data folder for the program (made if missing; empty if there is none). On Android, the application's
// own folder in shared storage (Android/data/<package>/files), which adb and a computer over USB reach.
nx::string user_folder(const Game& game) {
#if defined(__ANDROID__)
    (void)game;
    const char* external = SDL_GetAndroidExternalStoragePath();
    return external ? nx::string(external) : nx::string();
#else
    const nx::string app(game.name);
    char* pref = SDL_GetPrefPath("psvrecore", app.c_str());
    if (!pref) return {};
    nx::string path(pref);
    SDL_free(pref);
    while (!path.empty() && (path.back() == '/' || path.back() == '\\')) path.pop_back();
    return path;
#endif
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
        .option_many("env", c.environment, "an environment variable to set first (PSVRECORE_HLE_COUNTS=5, ...)",
                     {.value_name = "NAME=VALUE"})
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
    for (const nx::string& e : cl.environment) {
        const usize eq = e.view().find('=');
        if (eq == nx::string_view::npos || eq == 0) return fail(nx::format("--env {}: expected NAME=VALUE", e), true);
        const nx::string name(e.view().substr(0, eq)), value(e.view().substr(eq + 1));
#if defined(_WIN32)
        _putenv_s(name.c_str(), value.c_str());
#else
        setenv(name.c_str(), value.c_str(), 1);
#endif
    }
    if (cl.list_gpus) {
        auto gpus = vulkan::Device::list();
        if (!gpus) return fail(nx::format("Vulkan: {}", gpus.error().message), true);
        for (usize i = 0; i < gpus->size(); ++i) std::printf("%zu: %s\n", i, (*gpus)[i].c_str());
        return 0;
    }

    SDL_SetAppMetadata(title.c_str(), nullptr, nx::string(game.name).c_str());
    const nx::string user = user_folder(game);
#if defined(__ANDROID__)
    // (standard output and error reach nothing there: they go to output.txt in the user's folder)
    if (!user.empty()) {
        const nx::string output = join(user, "output.txt");
        if (std::freopen(output.c_str(), "w", stdout)) std::setvbuf(stdout, nullptr, _IOLBF, 0);
        if (std::freopen(output.c_str(), "a", stderr)) std::setvbuf(stderr, nullptr, _IONBF, 0);
    }
#endif
    nx::string settings_file;
    auto settings = resolve_settings(cl, user.empty() ? nx::string() : join(user, "settings.ini"), &settings_file);
    if (!settings) return fail(settings.error().message, cl.hidden);
#if defined(__ANDROID__)
    // (no command line there: the game's folder is game/ in the user's folder unless the settings say otherwise)
    // (made when missing, to show where the game's files go: a folder adb or a computer makes the application may
    // not be allowed to read)
    if (settings->game_folder.empty() && !user.empty()) {
        settings->game_folder = join(user, "game");
        (void)nx::fs::create_directories(nx::fs::path_view(settings->game_folder));
    }
#endif
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
    const auto layout = find_layout(s.game_folder, game.title_id);
    if (!layout) return fail(layout.error().message, cl.hidden);
    if (game.check) {
        if (auto r = game.check(layout->executable_folder()); !r) return fail(r.error().message, cl.hidden);
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
    window_options.fps_overlay = s.fps_overlay;
    window_options.gpu = s.gpu;
    window_options.validation = s.validation;
    window_options.hidden = cl.hidden;
    window_options.controls = static_cast<platform::TouchControls::Mode>(s.on_screen);
    window_options.controls_opacity = s.controls_opacity;
    window_options.controls_scale = s.controls_scale;
    auto window = platform::Window::open(window_options);
    if (!window) return fail(window.error().message, cl.hidden);
    nx::unique_ptr<platform::AudioDevice> speaker;
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
    // (the update's files over the game's; the add-ons, each opened by the game before it reads it)
    runtime::FileSystem::Mount app0{"app0", layout->executable_folder(), false};
    if (!layout->update.empty()) app0.under.push_back(layout->app);
    config.files.mounts.push_back(std::move(app0));
    config.files.mounts.push_back({"addcont0", layout->addcont, false, {}, true});
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
    gxm_options.cache_folder = join(data, "cache");
    gxm_options.render_scale = s.render_scale;
    runtime::Runtime rt(config);
    if (!rt.ok()) return fail("The runtime could not start (see the log).", cl.hidden);
    gxm::register_libraries(gxm_options);
    // (what earlier runs drew with, made now: nothing of it is translated or compiled while the game plays)
    const gxm::WarmUp warm = gxm::warm_up();
    for (const PsvModuleCode* code : game.code) rt.loader().add_code(*code);
    if (game.prepare) game.prepare(rt);
    runtime::Loader::BootOptions boot;
    boot.main = nx::string(game.main);
    for (const char* m : game.modules) boot.modules.push_back(m);
    boot.args = {boot.main};
    auto main_thread = rt.loader().boot(boot);
    if (!main_thread) return fail(nx::format("The game could not start: {}", main_thread.error().message), cl.hidden);
    nx::logi(logs::frontend, "{} started from {}{}{}; data in {}", game.title, layout->app,
             layout->update.empty() ? nx::string() : nx::format(", update {}", layout->update),
             layout->addcont.empty() ? nx::string() : nx::format(", add-ons {}", layout->addcont), data);

    Lifecycle lifecycle;
    lifecycle.rt = &rt;
    lifecycle.audio = speaker.get();
    lifecycle.window = window->get();
    lifecycle.sound.store(s.audio);
    SDL_AddEventWatch(on_app_event, &lifecycle);
    g_lifecycle.store(&lifecycle);

    // Until the window is closed, the game exits, the script quits or the time is up.
    Script script(std::move(script_items));
    {
        nx::scoped_lock<nx::mutex> held(display.lock);
        display.probes = script.probes();
    }
    const u64 booted = nx::os::ticks_ns();
    if (!script.finished())
        nx::logi(logs::frontend, "script: its time 0 is the kernel clock's {:.3f} s", static_cast<f64>(rt.kernel().clock_ns()) / 1e9);
    u64 saved_at = booted;
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
        const bool go_on = script.play(
            ms, window->get(),
            [&](const nx::string& name) {
                nx::scoped_lock<nx::mutex> held(display.lock);
                display.shots.push_back(name);
            },
            [&](f32 x, f32 y) -> std::optional<u32> {
                nx::scoped_lock<nx::mutex> held(display.lock);
                for (usize i = 0; i < display.probes.size() && i < display.probed.size(); ++i)
                    if (display.probes[i].first == x && display.probes[i].second == y) return display.probed[i];
                return std::nullopt;
            });
        if (!go_on) {
            ended = "the script quit";
            break;
        }
        if (!(*window)->pump(&rt, 5)) break;
        if ((*window)->input().take_menu_request()) open_settings();
        if (nx::os::ticks_ns() - saved_at > 30'000'000'000ull) {
            gxm::save_caches();  // (now and then: the pipelines made since)
            saved_at = nx::os::ticks_ns();
        }
        if (g_settings_changed.exchange(false)) {
            // (a settings screen changed the file: what applies while playing, at once)
            nx::string used;
            if (auto fresh = resolve_settings(cl, settings_file, &used)) apply_live(*fresh, lifecycle);
            else nx::logw(logs::frontend, "settings: {}", fresh.error().message);
        }
    }
    const i32 code = rt.exited() ? rt.exit_code() : 0;
    gxm::save_caches();
    nx::log::flush();
    const gxm::PlayStats made = gxm::play_stats();
    std::fprintf(stderr,
                 "%s: %s; %llu frames presented, exit code %d; shaders: %u from the cache before the start "
                 "(%u pipelines, %.0f ms), %u translated and %u pipelines made while playing\n",
                 nx::string(game.name).c_str(), ended, static_cast<unsigned long long>((*window)->presented()), code,
                 warm.shaders, warm.pipelines, warm.ms, made.translated, made.pipelines);
    std::fflush(stdout);
    std::fflush(stderr);
    // (guest threads are not waited for: the process ends here)
    std::_Exit(code);
}

}  // namespace psvrecore::frontend

#if defined(__ANDROID__)
#include <jni.h>

namespace {

nx::string from_java(JNIEnv* env, jstring s) {
    if (!s) return {};
    const char* chars = env->GetStringUTFChars(s, nullptr);
    nx::string out(chars ? chars : "");
    if (chars) env->ReleaseStringUTFChars(s, chars);
    return out;
}

}  // namespace

// SettingsActivity: the settings file's keys, one a line, tab-separated: section, key, kind, choices (comma-separated)
// or the range (lo,hi), live (1/0), desktop only (1/0), help, value.
extern "C" JNIEXPORT jstring JNICALL Java_io_github_adamfull_psvrecore_SettingsActivity_nativeSchema(JNIEnv* env, jclass,
                                                                                                    jstring file) {
    using namespace psvrecore::frontend;
    Settings s;
    const nx::string path = from_java(env, file);
    if (auto text = nx::fs::file_read_text(nx::fs::path_view(path))) (void)apply_file_text(&s, text->view());
    nx::string out;
    static constexpr const char* kKinds[] = {"bool", "int", "float", "choice", "text", "folder"};
    for (const SettingInfo& i : describe(s)) {
        nx::string extra;
        if (i.kind == SettingInfo::Kind::Choice) {
            for (usize c = 0; c < i.choices.size(); ++c) extra += (c ? "," : "") + i.choices[c];
        } else if (i.kind == SettingInfo::Kind::Int || i.kind == SettingInfo::Kind::Float) {
            extra = nx::format("{},{}", i.lo, i.hi);
        }
        out += nx::format("{}\t{}\t{}\t{}\t{}\t{}\t{}\t{}\n", i.section, i.key, kKinds[static_cast<u32>(i.kind)], extra,
                          i.live ? 1 : 0, i.desktop_only ? 1 : 0, i.help, i.value);
    }
    return env->NewStringUTF(out.c_str());
}

// SettingsActivity: one value changed in the file; null, or why it was refused.
extern "C" JNIEXPORT jstring JNICALL Java_io_github_adamfull_psvrecore_SettingsActivity_nativeSet(JNIEnv* env, jclass,
                                                                                                 jstring file,
                                                                                                 jstring section,
                                                                                                 jstring key,
                                                                                                 jstring value) {
    auto r = psvrecore::frontend::set_in_file(from_java(env, file), from_java(env, section).view(),
                                              from_java(env, key).view(), from_java(env, value).view());
    if (!r) return env->NewStringUTF(r.error().message.c_str());
    psvrecore::frontend::g_settings_changed.store(true);
    return nullptr;
}

// PsvrecoreActivity's pause and resume (on the system's thread).
extern "C" JNIEXPORT void JNICALL
Java_io_github_adamfull_psvrecore_PsvrecoreActivity_nativeSetBackground(JNIEnv*, jclass, jboolean background) {
    if (auto* l = psvrecore::frontend::g_lifecycle.load()) l->set_background(background != JNI_FALSE);
}
#endif
