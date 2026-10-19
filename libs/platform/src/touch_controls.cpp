#include "psvrecore/platform/touch_controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace psvrecore::platform {

namespace {

// SCE_CTRL_* bits.
constexpr u32 kSelect = 1u << 0, kStart = 1u << 3, kUp = 1u << 4, kRight = 1u << 5, kDown = 1u << 6,
              kLeft = 1u << 7, kL = 1u << 8, kR = 1u << 9, kTriangle = 1u << 12, kCircle = 1u << 13,
              kCross = 1u << 14, kSquare = 1u << 15;

// ---- The atlas: white shapes on clear, anti-aliased, in cells of kCell pixels ----------------------------------

constexpr u32 kCell = 128, kColumns = 7, kRows = 3;
constexpr u32 kAtlasW = kCell * kColumns, kAtlasH = kCell * kRows;

// The sprites by cell (the pill is two cells wide).
enum Sprite : u8 {
    kDisc, kRing, kTriangleGlyph, kCircleGlyph, kCrossGlyph, kSquareGlyph, kArrowUp,  // row 0
    kArrowRight, kTextL, kTextR, kPill, kPillEnd, kTextStart, kTextSelect,              // row 1
    kMenuIcon,                                                                          // row 2
};

struct Vec {
    f32 x, y;
};

f32 length(Vec v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// The distance from p to the segment a-b.
f32 segment(Vec p, Vec a, Vec b) {
    const Vec ab{b.x - a.x, b.y - a.y}, ap{p.x - a.x, p.y - a.y};
    const f32 t = std::clamp((ap.x * ab.x + ap.y * ab.y) / (ab.x * ab.x + ab.y * ab.y), 0.0f, 1.0f);
    return length({ap.x - ab.x * t, ap.y - ab.y * t});
}

// The signed distance from p to a convex polygon (negative inside; corners counter-clockwise or clockwise).
f32 polygon(Vec p, const Vec* v, u32 n) {
    f32 d = 1e9f;
    bool inside = true;
    f32 sign = 0;
    for (u32 i = 0; i < n; ++i) {
        const Vec a = v[i], b = v[(i + 1) % n];
        d = std::min(d, segment(p, a, b));
        const f32 cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (sign == 0) sign = cross;
        else if ((cross < 0) != (sign < 0)) inside = false;
    }
    return inside ? -d : d;
}

// A 5 x 7 font for the labels.
const char* glyph(char c) {
    switch (c) {
        case 'L': return "10000100001000010000100001000011111";
        case 'R': return "11110100011000111110101001001010001";
        case 'S': return "01111100001000001110000010000111110";
        case 'T': return "11111001000010000100001000010000100";
        case 'A': return "01110100011000111111100011000110001";
        case 'E': return "11111100001000011110100001000011111";
        case 'C': return "01111100001000010000100001000001111";
        default: return "00000000000000000000000000000000000";
    }
}

class Atlas {
public:
    nx::vector<u8> pixels = nx::vector<u8>(usize{kAtlasW} * kAtlasH * 4, 0);

    // Fills a cell (or `cells` side by side) with a shape given as a signed distance in pixels from the cell's centre.
    template <typename F>
    void shape(u32 cell, F distance, u32 cells = 1) {
        const u32 cx = cell % kColumns * kCell, cy = cell / kColumns * kCell;
        const f32 half_w = kCell * cells * 0.5f, half_h = kCell * 0.5f;
        for (u32 y = 0; y < kCell; ++y) {
            for (u32 x = 0; x < kCell * cells; ++x) {
                const f32 d = distance(Vec{x + 0.5f - half_w, y + 0.5f - half_h});
                put(cx + x, cy + y, std::clamp(0.5f - d, 0.0f, 1.0f));
            }
        }
    }

    // Text in a cell, from the 5 x 7 font, `scale` pixels a dot, centred.
    void text(u32 cell, const char* s, u32 scale) {
        const u32 n = static_cast<u32>(std::strlen(s));
        const u32 w = n * 6 * scale - scale, h = 7 * scale;
        const u32 x0 = cell % kColumns * kCell + (kCell - w) / 2, y0 = cell / kColumns * kCell + (kCell - h) / 2;
        for (u32 i = 0; i < n; ++i) {
            const char* g = glyph(s[i]);
            for (u32 gy = 0; gy < 7; ++gy)
                for (u32 gx = 0; gx < 5; ++gx)
                    if (g[gy * 5 + gx] == '1')
                        for (u32 y = 0; y < scale; ++y)
                            for (u32 x = 0; x < scale; ++x)
                                put(x0 + (i * 6 + gx) * scale + x, y0 + gy * scale + y, 1.0f);
        }
    }

private:
    void put(u32 x, u32 y, f32 coverage) {
        u8* p = &pixels[(usize{y} * kAtlasW + x) * 4];
        const u8 a = static_cast<u8>(std::lround(coverage * 255.0f));
        p[0] = p[1] = p[2] = 255;
        p[3] = std::max(p[3], a);
    }
};

// A sprite's texture rectangle (the pill: two cells).
void sprite_uv(Sprite s, Quad* q) {
    const u32 cells = s == kPill ? 2 : 1;
    q->u0 = static_cast<f32>(s % kColumns * kCell) / kAtlasW;
    q->v0 = static_cast<f32>(s / kColumns * kCell) / kAtlasH;
    q->u1 = q->u0 + static_cast<f32>(kCell * cells) / kAtlasW;
    q->v1 = q->v0 + static_cast<f32>(kCell) / kAtlasH;
}

struct Color {
    f32 r, g, b, a;
};

}  // namespace

void TouchControls::atlas(nx::vector<u8>* rgba, u32* width, u32* height) {
    Atlas a;
    const f32 r = kCell * 0.46f, line = kCell * 0.07f;
    a.shape(kDisc, [&](Vec p) { return length(p) - r; });
    a.shape(kRing, [&](Vec p) { return std::fabs(length(p) - (r - line)) - line * 0.6f; });
    {
        const f32 g = kCell * 0.24f;  // (the glyphs' size)
        const Vec tri[3] = {{0, -g}, {g * 0.95f, g * 0.7f}, {-g * 0.95f, g * 0.7f}};
        a.shape(kTriangleGlyph, [&](Vec p) { return std::fabs(polygon(p, tri, 3)) - line * 0.5f; });
        a.shape(kCircleGlyph, [&](Vec p) { return std::fabs(length(p) - g * 0.85f) - line * 0.5f; });
        a.shape(kCrossGlyph, [&](Vec p) {
            return std::min(segment(p, {-g * 0.8f, -g * 0.8f}, {g * 0.8f, g * 0.8f}),
                            segment(p, {-g * 0.8f, g * 0.8f}, {g * 0.8f, -g * 0.8f})) -
                   line * 0.5f;
        });
        a.shape(kSquareGlyph, [&](Vec p) {
            const f32 s = g * 0.75f;
            const f32 box = std::max(std::fabs(p.x) - s, std::fabs(p.y) - s);
            return std::fabs(box) - line * 0.5f;
        });
        const Vec up[3] = {{0, -g}, {g, g * 0.6f}, {-g, g * 0.6f}};
        a.shape(kArrowUp, [&](Vec p) { return polygon(p, up, 3); });
        const Vec right[3] = {{g, 0}, {-g * 0.6f, g}, {-g * 0.6f, -g}};
        a.shape(kArrowRight, [&](Vec p) { return polygon(p, right, 3); });
    }
    a.text(kTextL, "L", 8);
    a.text(kTextR, "R", 8);
    a.shape(kPill, [&](Vec p) {
        const f32 hw = kCell - line, hh = kCell * 0.46f - line, round = hh;
        const Vec q{std::max(std::fabs(p.x) - hw + round, 0.0f), std::max(std::fabs(p.y) - hh + round, 0.0f)};
        return length(q) - round;
    }, 2);
    a.shape(kMenuIcon, [&](Vec p) {
        // (three bars)
        const f32 w = kCell * 0.26f, gap = kCell * 0.15f;
        f32 d = 1e9f;
        for (i32 i = -1; i <= 1; ++i) d = std::min(d, segment(p, {-w, gap * i}, {w, gap * i}));
        return d - line * 0.55f;
    });
    a.text(kTextStart, "START", 4);
    a.text(kTextSelect, "SELECT", 3);
    *rgba = std::move(a.pixels);
    *width = kAtlasW, *height = kAtlasH;
}

// ---- Layout and fingers ----------------------------------------------------------------------------------------

void TouchControls::set_mode(Mode mode) {
    mode_ = mode;
    const bool v = mode == Mode::Always;
    if (v != visible_) visible_ = v, changed();
}

void TouchControls::set_opacity(f32 opacity) {
    opacity_ = std::clamp(opacity, 0.0f, 1.0f);
    changed();
}

void TouchControls::set_scale(f32 scale) {
    scale_ = std::clamp(scale, 0.5f, 2.0f);
    changed();
}

void TouchControls::layout(f32 width, f32 height, f32 pixels_per_unit, f32 sx, f32 sy, f32 sw, f32 sh) {
    width_ = width, height_ = height, ppu_ = pixels_per_unit;
    if (sw <= 0 || sh <= 0) sx = 0, sy = 0, sw = width, sh = height;
    const f32 u = std::min(sh * 0.95f, sw * 0.5f) / 7.5f * scale_;
    unit_ = u;
    const f32 left = sx, right = sx + sw, top = sy, bottom = sy + sh, middle = sx + sw * 0.5f;
    dpad_x_ = left + 2.1f * u, dpad_y_ = bottom - 2.0f * u;
    face_x_ = right - 2.1f * u, face_y_ = bottom - 2.0f * u;
    stick_x_[0] = left + 2.1f * u, stick_y_[0] = bottom - 5.1f * u;
    stick_x_[1] = right - 2.1f * u, stick_y_[1] = bottom - 5.1f * u;
    const f32 rects[5][4] = {{left + 0.4f * u, top + 0.3f * u, left + 2.9f * u, top + 1.1f * u},
                             {right - 2.9f * u, top + 0.3f * u, right - 0.4f * u, top + 1.1f * u},
                             {middle - 2.3f * u, bottom - 0.95f * u, middle - 0.3f * u, bottom - 0.3f * u},
                             {middle + 0.3f * u, bottom - 0.95f * u, middle + 2.3f * u, bottom - 0.3f * u},
                             {middle - 0.6f * u, top + 0.3f * u, middle + 0.6f * u, top + 1.1f * u}};
    std::memcpy(rect_, rects, sizeof rect_);
    changed();
}

void TouchControls::touched() {
    ever_touched_ = true;
    if (mode_ == Mode::Auto && !visible_) visible_ = true, changed();
}

void TouchControls::gamepad_connected() { other_input(); }

void TouchControls::gamepads_gone(bool touch_screen) {
    if (mode_ == Mode::Auto && !visible_ && (touch_screen || ever_touched_)) visible_ = true, changed();
}

void TouchControls::other_input() {
    if (mode_ == Mode::Auto && visible_ && fingers_.empty()) visible_ = false, changed();
}

TouchControls::Control TouchControls::hit(f32 x, f32 y) const {
    // (the nearest, by distance over its reach)
    Control best = Control::None;
    f32 best_score = 1.0f;
    auto round = [&](Control c, f32 cx, f32 cy, f32 reach) {
        const f32 score = length({x - cx, y - cy}) / reach;
        if (score < best_score) best = c, best_score = score;
    };
    round(Control::Dpad, dpad_x_, dpad_y_, 1.9f * unit_);
    round(Control::Face, face_x_, face_y_, 1.9f * unit_);
    round(Control::LeftStick, stick_x_[0], stick_y_[0], 1.5f * unit_);
    round(Control::RightStick, stick_x_[1], stick_y_[1], 1.5f * unit_);
    const f32 m = 0.25f * unit_;
    for (u32 i = 0; i < 5; ++i) {
        if (x >= rect_[i][0] - m && x < rect_[i][2] + m && y >= rect_[i][1] - m && y < rect_[i][3] + m)
            return static_cast<Control>(static_cast<u32>(Control::L) + i);
    }
    return best;
}

void TouchControls::press(Finger& f, f32 x, f32 y) {
    const f32 u = unit_;
    switch (f.control) {
        case Control::Dpad: {
            // (eight ways: a diagonal presses both of its directions)
            const f32 dx = x - dpad_x_, dy = y - dpad_y_;
            f.bits = 0;
            if (length({dx, dy}) < 0.3f * u) break;
            const f32 a = std::atan2(dy, dx) * 57.29578f;  // (degrees, y down)
            if (std::fabs(a) < 67.5f) f.bits |= kRight;
            if (std::fabs(a) > 112.5f) f.bits |= kLeft;
            if (a > 22.5f && a < 157.5f) f.bits |= kDown;
            if (a < -22.5f && a > -157.5f) f.bits |= kUp;
            break;
        }
        case Control::Face: {
            const f32 d = 1.05f * u;
            const struct {
                f32 x, y;
                u32 bit;
            } buttons[4] = {{face_x_, face_y_ - d, kTriangle}, {face_x_ + d, face_y_, kCircle},
                            {face_x_, face_y_ + d, kCross}, {face_x_ - d, face_y_, kSquare}};
            f.bits = 0;
            f32 nearest = 1.0f * u;
            for (const auto& b : buttons) {
                const f32 dist = length({x - b.x, y - b.y});
                if (dist < nearest) nearest = dist, f.bits = b.bit;
            }
            break;
        }
        case Control::LeftStick:
        case Control::RightStick: {
            const u32 i = f.control == Control::RightStick;
            f32 vx = (x - stick_x_[i]) / (1.1f * u), vy = (y - stick_y_[i]) / (1.1f * u);
            const f32 len = length({vx, vy});
            if (len > 1) vx /= len, vy /= len;
            stick_[i][0] = static_cast<u8>(std::clamp(std::lround(128 + vx * 127), 0l, 255l));
            stick_[i][1] = static_cast<u8>(std::clamp(std::lround(128 + vy * 127), 0l, 255l));
            f.bits = 0;
            break;
        }
        default: {
            if (f.control == Control::Menu) {
                f.bits = 0;
                break;
            }
            const u32 i = static_cast<u32>(f.control) - static_cast<u32>(Control::L);
            const f32 m = 0.4f * u;  // (a little off it still presses)
            const bool on = x >= rect_[i][0] - m && x < rect_[i][2] + m && y >= rect_[i][1] - m && y < rect_[i][3] + m;
            static constexpr u32 kBits[4] = {kL, kR, kSelect, kStart};
            f.bits = on ? kBits[i] : 0;
            break;
        }
    }
}

bool TouchControls::finger_down(u64 id, f32 x, f32 y) {
    if (!visible_) return false;
    const Control c = hit(x, y);
    if (c == Control::None) return false;
    fingers_.push_back(Finger{id, c, 0});
    press(fingers_.back(), x, y);
    if (c == Control::Menu) menu_requested_ = true;
    changed();
    return true;
}

bool TouchControls::finger_move(u64 id, f32 x, f32 y) {
    for (Finger& f : fingers_) {
        if (f.id != id) continue;
        press(f, x, y);
        changed();
        return true;
    }
    return false;
}

bool TouchControls::finger_up(u64 id) {
    for (usize i = 0; i < fingers_.size(); ++i) {
        if (fingers_[i].id != id) continue;
        if (fingers_[i].control == Control::LeftStick || fingers_[i].control == Control::RightStick) {
            const u32 s = fingers_[i].control == Control::RightStick;
            stick_[s][0] = stick_[s][1] = 0x80;
        }
        fingers_.erase(fingers_.begin() + static_cast<std::ptrdiff_t>(i));
        changed();
        return true;
    }
    return false;
}

u32 TouchControls::buttons() const {
    u32 bits = 0;
    for (const Finger& f : fingers_) bits |= f.bits;
    return bits;
}

nx::vector<Quad> TouchControls::quads() const {
    nx::vector<Quad> out;
    if (!visible_) return out;
    const u32 pressed = buttons();
    const f32 k = opacity_;
    const Color body{0.08f, 0.08f, 0.1f, std::min(0.9f * k, 1.0f)}, lit{0.6f, 0.6f, 0.65f, std::min(1.2f * k, 1.0f)};
    const Color line{1, 1, 1, std::min(0.7f * k, 1.0f)}, label{1, 1, 1, std::min(1.6f * k, 1.0f)};
    auto add = [&](Sprite s, f32 cx, f32 cy, f32 hw, f32 hh, Color c, bool flip_x = false, bool flip_y = false) {
        Quad q;
        q.x0 = (cx - hw) * ppu_, q.y0 = (cy - hh) * ppu_, q.x1 = (cx + hw) * ppu_, q.y1 = (cy + hh) * ppu_;
        sprite_uv(s, &q);
        if (flip_x) std::swap(q.u0, q.u1);
        if (flip_y) std::swap(q.v0, q.v1);
        q.r = c.r, q.g = c.g, q.b = c.b, q.a = c.a;
        out.push_back(q);
    };
    const f32 u = unit_;
    // The d-pad: four pads with their arrows.
    add(kRing, dpad_x_, dpad_y_, 1.65f * u, 1.65f * u, Color{1, 1, 1, line.a * 0.4f});
    const struct {
        f32 dx, dy;
        u32 bit;
        Sprite arrow;
        bool fx, fy;
    } pads[4] = {{0, -1, kUp, kArrowUp, false, false}, {1, 0, kRight, kArrowRight, false, false},
                 {0, 1, kDown, kArrowUp, false, true}, {-1, 0, kLeft, kArrowRight, true, false}};
    for (const auto& p : pads) {
        const f32 x = dpad_x_ + p.dx * 1.0f * u, y = dpad_y_ + p.dy * 1.0f * u;
        add(kDisc, x, y, 0.55f * u, 0.55f * u, (pressed & p.bit) ? lit : body);
        add(p.arrow, x, y, 0.55f * u, 0.55f * u, label, p.fx, p.fy);
    }
    // The face buttons.
    const struct {
        f32 dx, dy;
        u32 bit;
        Sprite glyph;
        Color color;
    } faces[4] = {{0, -1, kTriangle, kTriangleGlyph, {0.35f, 0.95f, 0.75f, label.a}},
                  {1, 0, kCircle, kCircleGlyph, {1.0f, 0.42f, 0.42f, label.a}},
                  {0, 1, kCross, kCrossGlyph, {0.55f, 0.7f, 1.0f, label.a}},
                  {-1, 0, kSquare, kSquareGlyph, {1.0f, 0.6f, 0.9f, label.a}}};
    for (const auto& f : faces) {
        const f32 x = face_x_ + f.dx * 1.05f * u, y = face_y_ + f.dy * 1.05f * u;
        add(kDisc, x, y, 0.58f * u, 0.58f * u, (pressed & f.bit) ? lit : body);
        add(f.glyph, x, y, 0.58f * u, 0.58f * u, f.color);
    }
    // The sticks: a ring and its knob where the stick is.
    for (u32 i = 0; i < 2; ++i) {
        add(kRing, stick_x_[i], stick_y_[i], 1.15f * u, 1.15f * u, line);
        const f32 kx = stick_x_[i] + (stick_[i][0] - 128) / 127.0f * 1.1f * u;
        const f32 ky = stick_y_[i] + (stick_[i][1] - 128) / 127.0f * 1.1f * u;
        const bool held = stick_[i][0] != 0x80 || stick_[i][1] != 0x80;
        add(kDisc, kx, ky, 0.55f * u, 0.55f * u, held ? lit : body);
    }
    // L, R, Select, Start.
    static constexpr u32 kBits[4] = {kL, kR, kSelect, kStart};
    static constexpr Sprite kLabels[4] = {kTextL, kTextR, kTextSelect, kTextStart};
    for (u32 i = 0; i < 4; ++i) {
        const f32 cx = (rect_[i][0] + rect_[i][2]) * 0.5f, cy = (rect_[i][1] + rect_[i][3]) * 0.5f;
        const f32 hw = (rect_[i][2] - rect_[i][0]) * 0.5f, hh = (rect_[i][3] - rect_[i][1]) * 0.5f;
        add(kPill, cx, cy, hw, hh, (pressed & kBits[i]) ? lit : body);
        add(kLabels[i], cx, cy, hh * (i < 2 ? 1.2f : 1.6f), hh * (i < 2 ? 1.2f : 1.6f), label);
    }
    // The menu.
    {
        const f32 cx = (rect_[4][0] + rect_[4][2]) * 0.5f, cy = (rect_[4][1] + rect_[4][3]) * 0.5f;
        const f32 hw = (rect_[4][2] - rect_[4][0]) * 0.5f, hh = (rect_[4][3] - rect_[4][1]) * 0.5f;
        const bool held = std::any_of(fingers_.begin(), fingers_.end(),
                                      [](const Finger& f) { return f.control == Control::Menu; });
        add(kPill, cx, cy, hw, hh, held ? lit : body);
        add(kMenuIcon, cx, cy, hh * 1.3f, hh * 1.3f, label);
    }
    return out;
}

}  // namespace psvrecore::platform
