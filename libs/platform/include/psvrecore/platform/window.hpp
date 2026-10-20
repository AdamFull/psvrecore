#pragma once

// The window the game is shown in (SDL3), and the frames it displays drawn into it with Vulkan (gxm::DisplaySink).
//
// The game's thread only copies a frame it displays (SceDisplay's frame buffer, read from guest memory); the window's
// own thread presents the latest one, scaled to fit the window with black bars, on its own Vulkan device. Frames come
// as the game displays them and are never waited for: with vsync the window's thread waits for the host's display,
// and a frame the game displays meanwhile replaces the one waiting.
//
// The window is opened, and its events pumped, on one thread (the main one: SDL's rule on most systems); the controls
// in those events go to the runtime's input (InputMapper).

#include <atomic>
#include <memory>

#include "psvrecore/error.hpp"
#include "psvrecore/gxm/hle.hpp"
#include "psvrecore/platform/input.hpp"
#include "psvrecore/platform/quad.hpp"
#include "core/foundation/core/foundation.h"

struct SDL_Window;
struct SDL_Gamepad;

namespace psvrecore::runtime {
class Runtime;
}

namespace psvrecore::platform {

// The screen's size: frames are scaled from it, and the window's default size is a multiple of it.
inline constexpr u32 kScreenWidth = 960, kScreenHeight = 544;

class Window : public gxm::DisplaySink {
public:
    struct Options {
        nx::string title = "psvrecore";
        u32 scale = 1;           // the window's size in screens (960 x 544 each)
        bool fullscreen = false;
        bool vsync = true;       // present at the host display's refresh (MAILBOX or FIFO; else IMMEDIATE first)
        bool smooth = true;      // linear filtering when scaled (else nearest)
        i32 gpu = -1;            // vulkan::DeviceOptions::gpu
        bool validation = false;
        bool hidden = false;     // (tests)
        // The on-screen controls: shown once the screen is touched (Auto; at once on Android), always or never.
        TouchControls::Mode controls = TouchControls::Mode::Auto;
        f32 controls_opacity = 0.5f;
        f32 controls_scale = 1.0f;
        bool fps_overlay = false;  // the frame rate and a graph of the frame times over the picture
    };

    static Result<nx::unique_ptr<Window>> open(const Options& options);
    ~Window() override;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // On the thread that opened the window: handles the host's events (waiting up to `wait_ms` for the first), the
    // controls going to the runtime's input. False once the window has been closed (or the application asked to quit).
    bool pump(runtime::Runtime* rt, u32 wait_ms = 0);
    // (a test's events, as pump() takes them)
    void handle(const SDL_Event& event, runtime::Runtime* rt);

    // From the game's thread.
    void show(const gxm::Frame& frame) override;
    // A frame from host memory (`pitch` in pixels; `pixel_format` as gxm::Frame's), as show() takes it.
    void show_pixels(const u8* pixels, u32 width, u32 height, u32 pitch, u32 pixel_format);

    // Frames presented so far.
    u64 presented() const { return presented_.load(); }
    // Waits until `count` frames have been presented (false on timeout).
    bool wait_presented(u64 count, u32 timeout_ms);
    // The window's last presented image as RGBA (for tests: kept once `keep_last(true)`, where the swapchain allows).
    void keep_last(bool keep);
    bool last_image(nx::vector<u8>* rgba, u32* width, u32* height);

    SDL_Window* sdl() const { return window_; }
    // What is drawn over the picture (the on-screen controls): quads cut from one texture, the atlas (RGBA, straight
    // alpha). From any thread; the quads are in the window's pixels.
    void set_overlay(nx::vector<Quad> quads);
    void set_overlay_atlas(const u8* rgba, u32 width, u32 height);
    // Filtering the picture when it is scaled, changed while running.
    void set_smooth(bool smooth);
    // The frame rate overlay (its numbers over the last second, and the last frames' times as a graph) shown or not,
    // changed while running (F3 switches it on a desktop).
    void set_fps_overlay(bool on);
    bool fps_overlay() const;
    // The view and the on-screen controls laid out again (after their settings changed).
    void refresh_view() { update_view(); }
    // The application to the background (on Android: before its window's surface goes) and back: nothing is drawn
    // meanwhile. From any thread.
    void set_background(bool background);
    // The device the window presents with, for the GXM layer to render with too (gxm::Options::shared_device).
    vulkan::Device* device() const;
    InputMapper& input() { return input_; }

private:
    struct Presenter;
    template <typename T, typename... Args>
    friend nx::unique_ptr<T> nx::make_unique(Args&&...);
    Window() = default;
    void update_view();
    void update_overlay();

    SDL_Window* window_ = nullptr;
    Options options_;
    InputMapper input_;
    nx::vector<SDL_Gamepad*> gamepads_;
    nx::unique_ptr<Presenter> presenter_;
    std::atomic<u64> presented_{0};
    bool closed_ = false;
    u64 overlay_version_ = 0;
};

}  // namespace psvrecore::platform
