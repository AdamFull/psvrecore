// The platform layer: the host's controls as the console's (events made here), the audio ports on an audio device
// (SDL's disk driver, whose output is read back), and frames presented in a window (its image read back; skipped
// without a display or a Vulkan device).

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nxtest.h"
#include "psvrecore/platform/audio.hpp"
#include "psvrecore/platform/input.hpp"
#include "psvrecore/platform/touch_controls.hpp"
#include "psvrecore/platform/window.hpp"
#include "psvrecore/vulkan/device.hpp"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"
#include "core/foundation/threading/thread.h"

using namespace psvrecore;
using namespace psvrecore::platform;

namespace {

constexpr u32 kSelect = 1u << 0, kL3 = 1u << 1, kStart = 1u << 3, kUp = 1u << 4, kLeft = 1u << 7, kL = 1u << 8,
              kR = 1u << 9, kTriangle = 1u << 12, kCircle = 1u << 13, kCross = 1u << 14, kSquare = 1u << 15;

// The audio test's ramp: interleaved stereo, the right channel the left's negative.
i16 ramp_at(usize i) {
    const i16 v = static_cast<i16>(static_cast<i32>(i / 2 % 2000) * 10 + 100);
    return i % 2 ? static_cast<i16>(-v) : v;
}

nx::string temp_folder() {
    if (auto t = nx::os::environment_variable("TEMP")) return *t;
    return "/tmp";
}

SDL_Event gamepad_button(SDL_JoystickID which, u8 button, bool down) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = which;
    e.gbutton.button = button;
    e.gbutton.down = down;
    return e;
}

SDL_Event gamepad_axis(SDL_JoystickID which, u8 axis, i16 value) {
    SDL_Event e{};
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = which;
    e.gaxis.axis = axis;
    e.gaxis.value = value;
    return e;
}

SDL_Event key(SDL_Keycode k, bool down, bool repeat = false) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = k;
    e.key.down = down;
    e.key.repeat = repeat;
    return e;
}

SDL_Event mouse_button(f32 x, f32 y, bool down, u8 button = SDL_BUTTON_LEFT) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    e.button.which = 1;
    e.button.button = button;
    e.button.down = down;
    e.button.x = x, e.button.y = y;
    return e;
}

SDL_Event mouse_motion(f32 x, f32 y) {
    SDL_Event e{};
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.which = 1;
    e.motion.x = x, e.motion.y = y;
    return e;
}

SDL_Event finger(Uint32 type, u64 id, f32 x, f32 y) {
    SDL_Event e{};
    e.type = type;
    e.tfinger.fingerID = id;
    e.tfinger.x = x, e.tfinger.y = y;
    return e;
}

}  // namespace

TEST_CASE("input: gamepad buttons by position, triggers as L and R, sticks to 0-255, several gamepads merged") {
    InputMapper m;
    runtime::Input input;
    CHECK(m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_SOUTH, true), input, 100));
    CHECK(m.pad().buttons == kCross);
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_EAST, true), input, 110);
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_WEST, true), input, 120);
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_NORTH, true), input, 130);
    CHECK(m.pad().buttons == (kCross | kCircle | kSquare | kTriangle));
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_SOUTH, false), input, 140);
    CHECK(m.pad().buttons == (kCircle | kSquare | kTriangle));
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_EAST, false), input, 150);
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_WEST, false), input, 150);
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_NORTH, false), input, 150);
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_BACK, true), input, 160);
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_START, true), input, 160);
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_LEFT_STICK, true), input, 160);
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_DPAD_UP, true), input, 160);
    m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true), input, 160);
    CHECK(m.pad().buttons == (kSelect | kStart | kL3 | kUp | kL));
    // (a button the console has no counterpart for changes nothing)
    CHECK_FALSE(m.handle(gamepad_button(1, SDL_GAMEPAD_BUTTON_GUIDE, true), input, 170));

    // Triggers past half way press R (and L, already down from the shoulder, stays down when its trigger lets go).
    m.handle(gamepad_axis(1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 20000), input, 180);
    CHECK((m.pad().buttons & kR) != 0);
    m.handle(gamepad_axis(1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 10000), input, 190);
    CHECK((m.pad().buttons & kR) == 0);
    m.handle(gamepad_axis(1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 32767), input, 190);
    m.handle(gamepad_axis(1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0), input, 195);
    CHECK((m.pad().buttons & kL) != 0);

    m.handle(gamepad_axis(1, SDL_GAMEPAD_AXIS_LEFTX, -32768), input, 200);
    m.handle(gamepad_axis(1, SDL_GAMEPAD_AXIS_LEFTY, 32767), input, 200);
    m.handle(gamepad_axis(1, SDL_GAMEPAD_AXIS_RIGHTX, 0), input, 200);
    m.handle(gamepad_axis(1, SDL_GAMEPAD_AXIS_RIGHTY, -16384), input, 200);
    CHECK(m.pad().lx == 0);
    CHECK(m.pad().ly == 255);
    CHECK(m.pad().rx == 128);
    CHECK(m.pad().ry == 64);

    // A second gamepad: its buttons add, its stick wins where it is further from rest.
    m.handle(gamepad_button(2, SDL_GAMEPAD_BUTTON_SOUTH, true), input, 210);
    m.handle(gamepad_axis(2, SDL_GAMEPAD_AXIS_RIGHTX, 32767), input, 210);
    m.handle(gamepad_axis(2, SDL_GAMEPAD_AXIS_LEFTX, 8192), input, 210);
    CHECK((m.pad().buttons & kCross) != 0);
    CHECK(m.pad().rx == 255);
    CHECK(m.pad().lx == 0);
    // Removing the first leaves the second's.
    SDL_Event removed{};
    removed.type = SDL_EVENT_GAMEPAD_REMOVED;
    removed.gdevice.which = 1;
    m.handle(removed, input, 220);
    CHECK(m.pad().buttons == kCross);
    CHECK(m.pad().lx == 160);
    CHECK(m.pad().ly == 128);

    // The runtime's input has the changes at their times.
    const runtime::PadState at_130 = input.pad_sample(129, 130);
    CHECK(at_130.buttons == (kCross | kCircle | kSquare | kTriangle));
    CHECK(input.pad_sample(220, 1000).buttons == kCross);
}

TEST_CASE("input: the keyboard's keys, without repeats, combined with a gamepad's") {
    InputMapper m;
    runtime::Input input;
    CHECK(m.handle(key(SDLK_Z, true), input, 10));
    m.handle(key(SDLK_X, true), input, 10);
    m.handle(key(SDLK_A, true), input, 10);
    m.handle(key(SDLK_S, true), input, 10);
    m.handle(key(SDLK_LEFT, true), input, 10);
    m.handle(key(SDLK_Q, true), input, 10);
    m.handle(key(SDLK_W, true), input, 10);
    m.handle(key(SDLK_RETURN, true), input, 10);
    m.handle(key(SDLK_RSHIFT, true), input, 10);
    CHECK(m.pad().buttons == (kCross | kCircle | kSquare | kTriangle | kLeft | kL | kR | kStart | kSelect));
    CHECK_FALSE(m.handle(key(SDLK_Z, true, true), input, 20));  // (a repeat)
    CHECK_FALSE(m.handle(key(SDLK_F1, true), input, 20));       // (not a control)
    m.handle(gamepad_button(3, SDL_GAMEPAD_BUTTON_SOUTH, true), input, 30);
    m.handle(key(SDLK_Z, false), input, 40);
    CHECK((m.pad().buttons & kCross) != 0);  // (the gamepad's still down)
    m.handle(gamepad_button(3, SDL_GAMEPAD_BUTTON_SOUTH, false), input, 50);
    CHECK((m.pad().buttons & kCross) == 0);
}

TEST_CASE("input: the mouse and fingers touch the front panel where they are over the picture") {
    InputMapper m;
    runtime::Input input;
    // A window of 1200 x 544 with the picture (960 x 544) in its middle.
    m.set_view(1200, 544, 120, 0, 960, 544);
    CHECK_FALSE(m.handle(mouse_button(60, 272, true), input, 10));  // (on a bar: nothing)
    CHECK_FALSE(m.handle(mouse_motion(70, 272), input, 15));
    m.handle(mouse_button(60, 272, false), input, 18);
    CHECK(m.handle(mouse_button(120 + 480, 272, true), input, 20));
    REQUIRE(m.touch().count == 1);
    CHECK(m.touch().points[0].x == 960);
    CHECK(m.touch().points[0].y == 544);
    m.handle(mouse_motion(120 + 959.5f, 543.5f), input, 30);
    CHECK(m.touch().points[0].x == 1919);
    CHECK(m.touch().points[0].y == 1087);
    CHECK(input.touch_sample(0, 30).count == 1);
    // Dragging off the picture lifts it.
    CHECK(m.handle(mouse_motion(1150, 300), input, 40));
    CHECK(m.touch().count == 0);
    CHECK(input.touch_sample(0, 40).count == 0);
    // (other buttons do not touch)
    CHECK_FALSE(m.handle(mouse_button(600, 272, true, SDL_BUTTON_RIGHT), input, 50));

    // Fingers (in the window's 0-1 coordinates): each its own id while down.
    CHECK(m.handle(finger(SDL_EVENT_FINGER_DOWN, 77, 0.5f, 0.5f), input, 60));
    CHECK(m.handle(finger(SDL_EVENT_FINGER_DOWN, 78, 120.0f / 1200, 0.0f), input, 61));
    REQUIRE(m.touch().count == 2);
    const u8 first = m.touch().points[0].id, second = m.touch().points[1].id;
    CHECK(first != second);
    CHECK(m.touch().points[0].x == 960);
    CHECK(m.touch().points[1].x == 0);
    CHECK(m.touch().points[1].y == 0);
    m.handle(finger(SDL_EVENT_FINGER_MOTION, 77, 0.6f, 0.5f), input, 62);
    CHECK(m.touch().points[0].id == first);
    CHECK(m.touch().points[0].x == static_cast<i16>((0.6f * 1200 - 120) / 960 * 1920));
    m.handle(finger(SDL_EVENT_FINGER_UP, 77, 0.6f, 0.5f), input, 63);
    REQUIRE(m.touch().count == 1);
    CHECK(m.touch().points[0].id == second);
    CHECK_FALSE(m.handle(finger(SDL_EVENT_FINGER_DOWN, 79, 0.01f, 0.5f), input, 64));  // (on a bar)
    const runtime::TouchState t = input.touch_sample(0, 63);
    CHECK(t.count == 1);
}

TEST_CASE("audio: ports play on the device, mixed, as the blocks give them") {
    const nx::string path = nx::format("{}/psvrecore_audio_test_{}.raw", temp_folder(), nx::os::process_id());
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "disk");
    SDL_SetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE, path.c_str());
    {
        auto opened = AudioDevice::open();
        if (!opened) SKIP(nx::format("no disk audio device: {}", opened.error().message).c_str());
        AudioDevice& device = **opened;
        // Port 0: a ramp, stereo, 48 kHz (the device's: nothing converted). 0.25 s.
        nx::vector<i16> ramp(12000 * 2);
        for (usize i = 0; i < ramp.size(); ++i) ramp[i] = ramp_at(i);
        device.play(runtime::AudioBlock{0, 0, 48000, 2, 0, ramp});
        CHECK(device.queued(0) > 0);
        nx::this_thread::sleep_for(std::chrono::milliseconds(600));
        CHECK(device.queued(0) == 0);
        // Ports 0 and 1 together: 1000 and 2000, mixed to 3000.
        nx::vector<i16> a(4800 * 2, 1000), b(4800 * 2, 2000);
        device.play(runtime::AudioBlock{0, 0, 48000, 2, 0, a});
        device.play(runtime::AudioBlock{1, 0, 48000, 2, 0, b});
        nx::this_thread::sleep_for(std::chrono::milliseconds(400));
        device.port_released(0);
        device.port_released(1);
        CHECK(device.queued(0) == 0);
    }
    SDL_ResetHint(SDL_HINT_AUDIO_DRIVER);
    SDL_ResetHint(SDL_HINT_AUDIO_DISK_OUTPUT_FILE);

    std::FILE* f = std::fopen(path.c_str(), "rb");
    REQUIRE(f != nullptr);
    nx::vector<i16> out;
    i16 buf[4096];
    for (usize n; (n = std::fread(buf, 2, 4096, f)) > 0;) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    std::remove(path.c_str());
    // The ramp, sample for sample (at most 1 off: SDL mixes in floating point), after the silence before it.
    usize start = 0;
    while (start < out.size() && out[start] == 0) ++start;
    start &= ~usize{1};
    REQUIRE(out.size() >= start + 24000);
    i32 worst = 0;
    for (usize i = 0; i < 24000; ++i) worst = std::max(worst, std::abs(static_cast<i32>(out[start + i]) - ramp_at(i)));
    std::printf("audio: the ramp from frame %zu, at most %d off\n", start / 2, worst);
    CHECK(worst <= 1);
    // The two ports' blocks: 3000 for 0.1 s.
    usize mixed = 0;
    for (usize i = start + 24000; i < out.size(); i += 2)
        if (std::abs(out[i] - 3000) <= 1) ++mixed;
    CHECK(mixed >= 4700);
    CHECK(mixed <= 4800);
}

namespace {

// The window test's picture: four colours, one in each quadrant of the screen.
constexpr u8 kQuadrant[4][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 0}};

nx::vector<u8> quadrants() {
    nx::vector<u8> p(usize{kScreenWidth} * kScreenHeight * 4);
    for (u32 y = 0; y < kScreenHeight; ++y) {
        for (u32 x = 0; x < kScreenWidth; ++x) {
            const u8* c = kQuadrant[(y >= kScreenHeight / 2 ? 2 : 0) + (x >= kScreenWidth / 2 ? 1 : 0)];
            u8* o = &p[(usize{y} * kScreenWidth + x) * 4];
            o[0] = c[0], o[1] = c[1], o[2] = c[2], o[3] = 255;
        }
    }
    return p;
}

struct Shot {
    nx::vector<u8> rgba;
    u32 w = 0, h = 0;
    const u8* at(u32 x, u32 y) const { return &rgba[(usize{y} * w + x) * 4]; }
};

bool is(const u8* p, const u8* c) {
    for (u32 i = 0; i < 3; ++i)
        if (std::abs(static_cast<i32>(p[i]) - c[i]) > 2) return false;
    return true;
}

// Pumps the window's events until its image (as large as the window, in pixels) shows the picture fitted, with black
// around it; false after 3 s.
bool presented_fitted(Window& w, Shot* shot) {
    static constexpr u8 black[3] = {0, 0, 0};
    for (u32 i = 0; i < 3000; ++i) {
        w.pump(nullptr);
        int pw = 0, ph = 0;
        SDL_GetWindowSizeInPixels(w.sdl(), &pw, &ph);
        if (w.last_image(&shot->rgba, &shot->w, &shot->h) && shot->w == static_cast<u32>(pw) &&
            shot->h == static_cast<u32>(ph)) {
            const f32 scale = std::min(static_cast<f32>(pw) / kScreenWidth, static_cast<f32>(ph) / kScreenHeight);
            const f32 vw = kScreenWidth * scale, vh = kScreenHeight * scale;
            const f32 vx = (static_cast<f32>(pw) - vw) / 2, vy = (static_cast<f32>(ph) - vh) / 2;
            bool ok = true;
            for (u32 q = 0; q < 4; ++q) {
                const u32 x = static_cast<u32>(vx + vw * ((q & 1) ? 0.75f : 0.25f));
                const u32 y = static_cast<u32>(vy + vh * ((q & 2) ? 0.75f : 0.25f));
                ok = ok && is(shot->at(x, y), kQuadrant[q]);
            }
            if (vx >= 4) ok = ok && is(shot->at(1, shot->h / 2), black) && is(shot->at(shot->w - 2, shot->h / 2), black);
            if (vy >= 4) ok = ok && is(shot->at(shot->w / 2, 1), black) && is(shot->at(shot->w / 2, shot->h - 2), black);
            if (ok) return true;
        }
        SDL_Delay(1);
    }
    return false;
}

}  // namespace

TEST_CASE("window: a frame shown is presented scaled to fit the window, with black bars") {
    Window::Options o;
    o.title = "psvrecore test";
    o.hidden = true;
    auto opened = Window::open(o);
    if (!opened) SKIP(nx::format("no window: {}", opened.error().message).c_str());
    Window& w = **opened;
    w.keep_last(true);
    const nx::vector<u8> picture = quadrants();
    w.show_pixels(picture.data(), kScreenWidth, kScreenHeight, kScreenWidth, 0);
    REQUIRE(w.wait_presented(1, 3000));
    Shot shot;
    if (!w.last_image(&shot.rgba, &shot.w, &shot.h)) SKIP("the window's images cannot be read back");
    REQUIRE(presented_fitted(w, &shot));
    // At 1:1 (no scaling from the display's density), every pixel as shown.
    if (shot.w == kScreenWidth && shot.h == kScreenHeight) {
        usize differ = 0;
        for (usize i = 0; i < picture.size(); i += 4)
            if (!is(&shot.rgba[i], &picture[i])) ++differ;
        CHECK(differ == 0);
    }
    // Wider than the picture: bars left and right; taller: above and below.
    SDL_SetWindowSize(w.sdl(), 1400, 544);
    CHECK(presented_fitted(w, &shot));
    std::printf("window: %u x %u pixels for 1400 x 544\n", shot.w, shot.h);
    SDL_SetWindowSize(w.sdl(), 700, 700);
    CHECK(presented_fitted(w, &shot));
    // Frames shown later replace the picture (and the A2B10G10R10 format is read as well).
    nx::vector<u32> wide(usize{kScreenWidth} * kScreenHeight, (1023u << 20) | (0u << 10) | 1023u);  // (blue, red)
    w.show_pixels(reinterpret_cast<const u8*>(wide.data()), kScreenWidth, kScreenHeight, kScreenWidth, 0x50000000u);
    bool magenta = false;
    static constexpr u8 kMagenta[3] = {255, 0, 255};
    for (u32 i = 0; i < 3000 && !magenta; ++i) {
        w.pump(nullptr);
        magenta = w.last_image(&shot.rgba, &shot.w, &shot.h) && is(shot.at(shot.w / 2, shot.h / 2), kMagenta);
        SDL_Delay(1);
    }
    CHECK(magenta);
    std::printf("window: %llu frames presented\n", static_cast<unsigned long long>(w.presented()));
}

TEST_CASE("window: in the background nothing is presented; back, a new surface and swapchain present again") {
    Window::Options o;
    o.title = "psvrecore test";
    o.hidden = true;
    auto opened = Window::open(o);
    if (!opened) SKIP(nx::format("no window: {}", opened.error().message).c_str());
    Window& w = **opened;
    w.keep_last(true);
    const nx::vector<u8> picture = quadrants();
    w.show_pixels(picture.data(), kScreenWidth, kScreenHeight, kScreenWidth, 0);
    Shot shot;
    REQUIRE(w.wait_presented(1, 3000));
    if (!w.last_image(&shot.rgba, &shot.w, &shot.h)) SKIP("the window's images cannot be read back");
    REQUIRE(presented_fitted(w, &shot));
    const VkSurfaceKHR before = w.device()->surface();
    REQUIRE(before != VK_NULL_HANDLE);

    w.set_background(true);
    CHECK(w.device()->surface() == VK_NULL_HANDLE);
    const u64 count = w.presented();
    for (u32 i = 0; i < 20; ++i) {
        w.show_pixels(picture.data(), kScreenWidth, kScreenHeight, kScreenWidth, 0);
        w.pump(nullptr);
        SDL_Delay(5);
    }
    CHECK(w.presented() == count);

    w.set_background(false);
    w.show_pixels(picture.data(), kScreenWidth, kScreenHeight, kScreenWidth, 0);
    REQUIRE(w.wait_presented(count + 1, 3000));
    CHECK(w.device()->surface() != VK_NULL_HANDLE);
    CHECK(presented_fitted(w, &shot));
}

TEST_CASE("window: overlay quads are blended over the picture, cut from their atlas") {
    Window::Options o;
    o.title = "psvrecore test";
    o.hidden = true;
    auto opened = Window::open(o);
    if (!opened) SKIP(nx::format("no window: {}", opened.error().message).c_str());
    Window& w = **opened;
    w.keep_last(true);
    const nx::vector<u8> picture = quadrants();
    w.show_pixels(picture.data(), kScreenWidth, kScreenHeight, kScreenWidth, 0);
    Shot shot;
    REQUIRE(w.wait_presented(1, 3000));
    if (!w.last_image(&shot.rgba, &shot.w, &shot.h)) SKIP("the window's images cannot be read back");
    REQUIRE(presented_fitted(w, &shot));
    if (shot.w != kScreenWidth || shot.h != kScreenHeight) SKIP("the window is not at 1:1");
    // An atlas of two texels: white half transparent, and clear.
    const u8 atlas[] = {255, 255, 255, 128, 0, 0, 0, 0};
    w.set_overlay_atlas(atlas, 2, 1);
    Quad white;  // (over the red quadrant: the left texel only)
    white.x0 = 100, white.y0 = 100, white.x1 = 200, white.y1 = 200;
    white.u0 = 0.1f, white.u1 = 0.4f;
    Quad tinted = white;  // (over the green one, tinted blue)
    tinted.x0 = 600, tinted.x1 = 700;
    tinted.r = 0, tinted.g = 0;
    Quad clear = white;  // (the clear texel over the blue one: nothing)
    clear.y0 = 350, clear.y1 = 450;
    clear.u0 = 0.6f, clear.u1 = 0.9f;
    w.set_overlay({white, tinted, clear});
    static constexpr u8 kPink[3] = {255, 128, 128}, kTeal[3] = {0, 127, 128};
    bool seen = false;
    for (u32 i = 0; i < 3000 && !seen; ++i) {
        w.show_pixels(picture.data(), kScreenWidth, kScreenHeight, kScreenWidth, 0);
        w.pump(nullptr);
        seen = w.last_image(&shot.rgba, &shot.w, &shot.h) && is(shot.at(150, 150), kPink);
        SDL_Delay(1);
    }
    REQUIRE(seen);
    std::printf("overlay: %u %u %u over red, %u %u %u over green\n", shot.at(150, 150)[0], shot.at(150, 150)[1],
                shot.at(150, 150)[2], shot.at(650, 150)[0], shot.at(650, 150)[1], shot.at(650, 150)[2]);
    CHECK(is(shot.at(650, 150), kTeal));
    CHECK(is(shot.at(150, 400), kQuadrant[2]));
    CHECK(is(shot.at(250, 150), kQuadrant[0]));  // (outside the quads)
    w.set_overlay({});
    bool gone = false;
    for (u32 i = 0; i < 3000 && !gone; ++i) {
        w.show_pixels(picture.data(), kScreenWidth, kScreenHeight, kScreenWidth, 0);
        w.pump(nullptr);
        gone = w.last_image(&shot.rgba, &shot.w, &shot.h) && is(shot.at(150, 150), kQuadrant[0]);
        SDL_Delay(1);
    }
    CHECK(gone);
}

TEST_CASE("touch controls: buttons, slides, the d-pad's diagonals, sticks, auto showing and hiding") {
    TouchControls c;
    // A phone's 2410 x 1080 screen with a notch on the left (the safe area starts at 172).
    c.layout(2410, 1080, 1.0f, 172, 0, 2238, 1080);
    const f32 u = 1080 * 0.95f / 7.5f;
    const f32 face_x = 2410 - 2.1f * u, face_y = 1080 - 2.0f * u, dpad_x = 172 + 2.1f * u, dpad_y = 1080 - 2.0f * u;
    CHECK_FALSE(c.visible());  // (auto: until the screen is touched)
    CHECK_FALSE(c.finger_down(1, face_x, face_y + 1.05f * u));
    c.touched();
    REQUIRE(c.visible());
    const u64 v0 = c.version();
    // Cross, then the same finger slid to Circle.
    REQUIRE(c.finger_down(1, face_x, face_y + 1.05f * u));
    CHECK(c.buttons() == kCross);
    CHECK(c.version() != v0);
    REQUIRE(c.finger_move(1, face_x + 1.05f * u, face_y));
    CHECK(c.buttons() == kCircle);
    // A second finger on the d-pad, up and to the right: both.
    REQUIRE(c.finger_down(2, dpad_x + 0.8f * u, dpad_y - 0.8f * u));
    CHECK(c.buttons() == (kCircle | kUp | 1u << 5));
    REQUIRE(c.finger_move(2, dpad_x - u, dpad_y + 0.05f * u));
    CHECK(c.buttons() == (kCircle | kLeft));
    // (a gamepad or a key while fingers are down does not hide them)
    c.other_input();
    CHECK(c.visible());
    CHECK(c.finger_up(1));
    CHECK(c.finger_up(2));
    CHECK(c.buttons() == 0);
    // The left stick, dragged right past its edge: all the way, and back to rest when let go.
    const f32 stick_y = 1080 - 5.1f * u;
    REQUIRE(c.finger_down(3, dpad_x, stick_y));
    CHECK(c.lx() == 128);
    c.finger_move(3, dpad_x + 3 * u, stick_y);
    CHECK(c.lx() == 255);
    CHECK(c.ly() == 128);
    c.finger_up(3);
    CHECK(c.lx() == 128);
    // L at the top left, Start at the bottom middle: their bits.
    REQUIRE(c.finger_down(4, 172 + 1.6f * u, 0.7f * u));
    CHECK(c.buttons() == kL);
    c.finger_up(4);
    REQUIRE(c.finger_down(5, 172 + 2238 / 2.0f + 1.3f * u, 1080 - 0.6f * u));
    CHECK(c.buttons() == kStart);
    c.finger_up(5);
    // The middle of the screen is no control's.
    CHECK_FALSE(c.finger_down(6, 1205, 400));
    // The menu button, at the top in the middle: no button, a request for the settings (once).
    CHECK_FALSE(c.take_menu_request());
    REQUIRE(c.finger_down(7, 172 + 2238 / 2.0f, 0.7f * u));
    CHECK(c.buttons() == 0);
    CHECK(c.take_menu_request());
    CHECK_FALSE(c.take_menu_request());
    c.finger_up(7);
    // Drawn while shown; nothing once a gamepad is used.
    CHECK(c.quads().size() > 20);
    c.other_input();
    CHECK_FALSE(c.visible());
    CHECK(c.quads().empty());
    // Never: not even when touched.
    c.set_mode(TouchControls::Mode::Never);
    c.touched();
    CHECK_FALSE(c.visible());
    c.set_mode(TouchControls::Mode::Always);
    CHECK(c.visible());
    nx::vector<u8> atlas;
    u32 aw = 0, ah = 0;
    TouchControls::atlas(&atlas, &aw, &ah);
    CHECK(aw * ah * 4 == atlas.size());
    usize opaque = 0;
    for (usize i = 3; i < atlas.size(); i += 4) opaque += atlas[i] > 200;
    CHECK(opaque > aw * ah / 20);  // (shapes in it)
}

TEST_CASE("input: fingers on the on-screen controls press them; others touch the panel") {
    InputMapper m;
    runtime::Input input;
    m.set_view(2410, 1080, 252, 0, 1906, 1080);
    m.controls().layout(2410, 1080, 1.0f, 172, 0, 2238, 1080);
    const f32 u = 1080 * 0.95f / 7.5f;
    const f32 cross_x = 2410 - 2.1f * u, cross_y = 1080 - 2.0f * u + 1.05f * u;
    // The first touch shows them and, being on Cross, presses it; the panel is not touched.
    CHECK(m.handle(finger(SDL_EVENT_FINGER_DOWN, 10, cross_x / 2410, cross_y / 1080), input, 100));
    CHECK(m.controls().visible());
    CHECK(m.pad().buttons == kCross);
    CHECK(m.touch().count == 0);
    // Another finger in the middle of the picture touches the panel.
    CHECK(m.handle(finger(SDL_EVENT_FINGER_DOWN, 11, 0.5f, 0.5f), input, 110));
    CHECK(m.touch().count == 1);
    CHECK(m.pad().buttons == kCross);
    m.handle(finger(SDL_EVENT_FINGER_UP, 10, cross_x / 2410, cross_y / 1080), input, 120);
    CHECK(m.pad().buttons == 0);
    CHECK(input.pad_sample(99, 105).buttons == kCross);
    // A key press hides them (with no finger on them).
    m.handle(key(SDLK_Z, true), input, 130);
    CHECK_FALSE(m.controls().visible());
}
