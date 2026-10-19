// The rendering context: scenes, the state draws take, uniform buffers and draws. A draw is recorded with copies of
// what may change before the scene is rendered (its uniform data); the scene is rendered when it ends.

#include <bit>
#include <cmath>
#include <cstring>

#include "psvrecore/gxm/texture.hpp"
#include "psvrecore/log.hpp"
#include "state.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::gxm::hle {

namespace {

constexpr u32 kSceGxm = 0xF76B66BD;

runtime::GuestMemory& memory() { return runtime::runtime().memory(); }

Context* context_of(u32 guest) {
    auto& contexts = state().contexts;
    auto it = contexts.find(guest);
    return it == contexts.end() ? nullptr : it->second.get();
}

// (round to nearest even)
u16 to_half(f32 v) {
    const u32 x = std::bit_cast<u32>(v);
    const u32 sign = (x >> 16) & 0x8000u;
    const i32 exp = static_cast<i32>((x >> 23) & 0xFF);
    u32 mant = x & 0x7FFFFFu;
    if (exp == 0xFF) return static_cast<u16>(sign | 0x7C00u | (mant ? 0x200u | (mant >> 13) : 0));
    const i32 e = exp - 112;
    if (e >= 31) return static_cast<u16>(sign | 0x7C00u);
    if (e <= 0) {
        if (e < -10) return static_cast<u16>(sign);
        mant |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - e);
        u32 h = mant >> shift;
        const u32 rem = mant & ((1u << shift) - 1), half = 1u << (shift - 1);
        if (rem > half || (rem == half && (h & 1))) ++h;
        return static_cast<u16>(sign | h);
    }
    u32 h = (static_cast<u32>(e) << 10) | (mant >> 13);
    const u32 rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1))) ++h;
    return static_cast<u16>(sign | h);
}

// ---- Scenes ------------------------------------------------------------------------------------------------------

void set_default_region_and_viewport(Context& c, u32 x_max, u32 y_max) {
    DrawState& s = c.state;
    s.region_clip_mode = 0x80000000u;  // SCE_GXM_REGION_CLIP_OUTSIDE
    s.region_min[0] = s.region_min[1] = 0;
    s.region_max[0] = x_max;
    s.region_max[1] = y_max;
    s.viewport[0] = 0.5f * static_cast<f32>(1 + x_max);
    s.viewport[1] = 0.5f * static_cast<f32>(1 + x_max);
    s.viewport[2] = 0.5f * static_cast<f32>(1 + y_max);
    s.viewport[3] = -0.5f * static_cast<f32>(1 + y_max);
    s.viewport[4] = 0.5f;
    s.viewport[5] = 0.5f;
}

i32 begin_scene(u32 context, u32 flags, u32 target, Ptr<const ValidRegion> region, u32 color, u32 depth) {
    if (!context || !target) return err::kInvalidPointer;
    State& st = state();
    nx::scoped_lock<nx::mutex> held(st.lock);
    Context* c = context_of(context);
    auto t = st.render_targets.find(target);
    if (!c || t == st.render_targets.end()) return err::kInvalidPointer;
    if (c->deferred) return err::kInvalidValue;  // (scenes are the immediate context's)
    if (c->in_scene) return err::kWithinScene;
    Scene scene;
    scene.target = t->second.get();
    scene.flags = flags;
    scene.visibility_buffer = c->visibility_buffer;
    scene.region = region ? *region : ValidRegion{scene.target->width - 1, scene.target->height - 1};
    if (color && memory().valid(color, sizeof(ColorSurface))) {
        scene.color = *reinterpret_cast<const ColorSurface*>(memory().host(color));
        scene.has_color = (scene.color.modes >> 31) != 0;
    }
    if (depth && memory().valid(depth, sizeof(DepthStencilSurface))) {
        scene.depth = *reinterpret_cast<const DepthStencilSurface*>(memory().host(depth));
        scene.has_depth = (scene.depth.control >> 31) != 0;
    }
    c->scene = std::move(scene);
    c->in_scene = true;
    set_default_region_and_viewport(*c, c->scene.region.x_max, c->scene.region.y_max);
    return 0;
}

i32 sceGxmBeginScene(u32 context, u32 flags, u32 target, Ptr<const ValidRegion> region, u32 vertex_sync,
                     u32 fragment_sync, u32 color, u32 depth) {
    (void)vertex_sync, (void)fragment_sync;  // (scenes are rendered when they end: nothing waits)
    return begin_scene(context, flags, target, region, color, depth);
}

i32 sceGxmBeginSceneEx(u32 context, u32 flags, u32 target, Ptr<const ValidRegion> region, u32 vertex_sync,
                       u32 fragment_sync, u32 color, u32 load_depth, u32 store_depth) {
    (void)vertex_sync, (void)fragment_sync, (void)store_depth;
    return begin_scene(context, flags, target, region, color, load_depth);
}

i32 sceGxmEndScene(u32 context, Ptr<const Notification> vertex_notification, Ptr<const Notification> fragment_notification) {
    if (!context) return err::kInvalidPointer;
    State& st = state();
    Scene scene;
    Context* c;
    {
        nx::scoped_lock<nx::mutex> held(st.lock);
        c = context_of(context);
        if (!c) return err::kInvalidPointer;
        if (!c->in_scene) return err::kNotWithinScene;
        c->in_scene = false;
        scene = std::move(c->scene);
        c->scene = Scene{};
    }
    render_scene(*c, scene);
    notify(vertex_notification);
    notify(fragment_notification);
    return 0;
}

i32 sceGxmMidSceneFlush(u32 context, u32 flags, u32 sync, Ptr<const Notification> notification) {
    (void)flags, (void)sync;
    if (!context) return err::kInvalidPointer;
    notify(notification);
    return 0;
}

// ---- State -------------------------------------------------------------------------------------------------------

template <class F>
void with(u32 context, F&& f) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    if (Context* c = context_of(context)) f(*c);
}

void sceGxmSetVertexProgram(u32 context, u32 program) {
    with(context, [&](Context& c) {
        auto it = state().vertex_programs.find(program);
        c.vertex = it == state().vertex_programs.end() ? nullptr : it->second.get();
    });
}
void sceGxmSetFragmentProgram(u32 context, u32 program) {
    with(context, [&](Context& c) {
        auto it = state().fragment_programs.find(program);
        c.fragment = it == state().fragment_programs.end() ? nullptr : it->second.get();
    });
}

i32 sceGxmSetVertexStream(u32 context, u32 index, u32 data) {
    if (!context || !data) return err::kInvalidPointer;
    if (index >= 16) return err::kInvalidValue;
    with(context, [&](Context& c) { c.streams[index] = data; });
    return 0;
}

void sceGxmSetFrontDepthFunc(u32 context, u32 f) { with(context, [&](Context& c) { c.state.depth_func[0] = f; }); }
void sceGxmSetBackDepthFunc(u32 context, u32 f) { with(context, [&](Context& c) { c.state.depth_func[1] = f; }); }
void sceGxmSetFrontDepthWriteEnable(u32 context, u32 m) {
    with(context, [&](Context& c) { c.state.depth_write[0] = m == 0; });
}
void sceGxmSetBackDepthWriteEnable(u32 context, u32 m) {
    with(context, [&](Context& c) { c.state.depth_write[1] = m == 0; });
}
void stencil_func(u32 context, u32 side, u32 func, u32 fail, u32 depth_fail, u32 pass, u32 compare, u32 write) {
    with(context, [&](Context& c) {
        StencilState& s = c.state.stencil[side];
        s.func = func, s.fail = fail, s.depth_fail = depth_fail, s.pass = pass;
        s.compare_mask = static_cast<u8>(compare), s.write_mask = static_cast<u8>(write);
    });
}
void sceGxmSetFrontStencilFunc(u32 context, u32 func, u32 fail, u32 depth_fail, u32 pass, u32 compare, u32 write) {
    stencil_func(context, 0, func, fail, depth_fail, pass, compare, write);
}
void sceGxmSetBackStencilFunc(u32 context, u32 func, u32 fail, u32 depth_fail, u32 pass, u32 compare, u32 write) {
    stencil_func(context, 1, func, fail, depth_fail, pass, compare, write);
}
void sceGxmSetFrontStencilRef(u32 context, u32 ref) {
    with(context, [&](Context& c) { c.state.stencil[0].ref = static_cast<u8>(ref); });
}
void sceGxmSetBackStencilRef(u32 context, u32 ref) {
    with(context, [&](Context& c) { c.state.stencil[1].ref = static_cast<u8>(ref); });
}
void sceGxmSetFrontDepthBias(u32 context, i32 factor, i32 units) {
    with(context, [&](Context& c) { c.state.depth_bias_factor[0] = factor, c.state.depth_bias_units[0] = units; });
}
void sceGxmSetBackDepthBias(u32 context, i32 factor, i32 units) {
    with(context, [&](Context& c) { c.state.depth_bias_factor[1] = factor, c.state.depth_bias_units[1] = units; });
}
void sceGxmSetFrontPolygonMode(u32 context, u32 m) { with(context, [&](Context& c) { c.state.polygon_mode[0] = m; }); }
void sceGxmSetBackPolygonMode(u32 context, u32 m) { with(context, [&](Context& c) { c.state.polygon_mode[1] = m; }); }
void sceGxmSetFrontPointLineWidth(u32 context, u32 w) {
    with(context, [&](Context& c) { c.state.point_line_width[0] = w; });
}
void sceGxmSetBackPointLineWidth(u32 context, u32 w) {
    with(context, [&](Context& c) { c.state.point_line_width[1] = w; });
}
void sceGxmSetFrontFragmentProgramEnable(u32 context, u32 m) {
    with(context, [&](Context& c) { c.state.fragment_enabled[0] = m == 0; });
}
void sceGxmSetBackFragmentProgramEnable(u32 context, u32 m) {
    with(context, [&](Context& c) { c.state.fragment_enabled[1] = m == 0; });
}
void sceGxmSetFrontLineFillLastPixelEnable(u32, u32) {}
void sceGxmSetBackLineFillLastPixelEnable(u32, u32) {}
void sceGxmSetCullMode(u32 context, u32 m) { with(context, [&](Context& c) { c.state.cull = m; }); }

// Visibility testing: the back face's settings follow the front's unless two-sided rendering is enabled (render.cpp
// reads the front's only: one query per draw).
void sceGxmSetFrontVisibilityTestEnable(u32 context, u32 m) {
    with(context, [&](Context& c) { c.state.visibility[0] = m == 0x4000; });
}
void sceGxmSetBackVisibilityTestEnable(u32 context, u32 m) {
    with(context, [&](Context& c) { c.state.visibility[1] = m == 0x4000; });
}
void sceGxmSetFrontVisibilityTestIndex(u32 context, u32 index) {
    with(context, [&](Context& c) { c.state.visibility_index[0] = index & 0x3FFF; });
}
void sceGxmSetBackVisibilityTestIndex(u32 context, u32 index) {
    with(context, [&](Context& c) { c.state.visibility_index[1] = index & 0x3FFF; });
}
void sceGxmSetFrontVisibilityTestOp(u32 context, u32 op) {
    with(context, [&](Context& c) { c.state.visibility_set[0] = op == 0x40000; });
}
void sceGxmSetBackVisibilityTestOp(u32 context, u32 op) {
    with(context, [&](Context& c) { c.state.visibility_set[1] = op == 0x40000; });
}

i32 sceGxmSetVisibilityBuffer(u32 context, u32 base, u32 stride) {
    if (!context || !base) return err::kInvalidPointer;
    if ((base | stride) & 15) return err::kInvalidAlignment;
    nx::scoped_lock<nx::mutex> held(state().lock);
    Context* c = context_of(context);
    if (!c) return err::kInvalidPointer;
    if (c->in_scene) return err::kWithinScene;
    c->visibility_buffer = base;
    return 0;
}
void sceGxmSetTwoSidedEnable(u32 context, u32 m) {
    with(context, [&](Context& c) { c.state.two_sided = m == 0x800; });
}
void sceGxmSetRegionClip(u32 context, u32 mode, u32 x0, u32 y0, u32 x1, u32 y1) {
    with(context, [&](Context& c) {
        c.state.region_clip_mode = mode;
        c.state.region_min[0] = x0, c.state.region_min[1] = y0;
        c.state.region_max[0] = x1, c.state.region_max[1] = y1;
    });
}
void sceGxmSetDefaultRegionClipAndViewport(u32 context, u32 x_max, u32 y_max) {
    with(context, [&](Context& c) { set_default_region_and_viewport(c, x_max, y_max); });
}
void sceGxmSetViewport(u32 context, f32 xo, f32 xs, f32 yo, f32 ys, f32 zo, f32 zs) {
    with(context, [&](Context& c) {
        const f32 v[6] = {xo, xs, yo, ys, zo, zs};
        std::memcpy(c.state.viewport, v, sizeof v);
    });
}
void sceGxmSetViewportEnable(u32 context, u32 m) {
    with(context, [&](Context& c) { c.state.viewport_enabled = m == 0; });
}
void sceGxmSetWClampValue(u32 context, f32 v) { with(context, [&](Context& c) { c.state.w_clamp = v; }); }
void sceGxmSetWClampEnable(u32 context, u32 m) {
    with(context, [&](Context& c) { c.state.w_clamp_enabled = m == 0x8000; });
}
void sceGxmSetWBufferEnable(u32 context, u32 m) {
    with(context, [&](Context& c) { c.state.w_buffer = m == 0x4000; });
}
// The YUV profiles are the device's, not each context's (games set them once). (User markers, validation: not
// modelled yet.)
i32 sceGxmSetYuvProfile(u32, u32 csc, u32 profile) {
    if (csc > 1 || profile > 3) return err::kInvalidValue;
    gxm::set_yuv_profile(csc, profile);
    return 0;
}
i32 sceGxmSetUserMarker(u32, u32) { return 0; }
void sceGxmSetValidationEnable(u32, u32) {}

// ---- Uniform buffers and textures ------------------------------------------------------------------------------

// A default uniform buffer from the context's vertex or fragment ring buffer.
// A deferred context's: from its vertex or fragment buffer, which the game's callback refills (called without the
// lock held: it is guest code).
i32 reserve_deferred(PsvContext* ctx, u32 context, bool vertex, u32 size, Ptr<u32> out) {
    State& st = state();
    u32 callback = 0, user = 0, scratch = 0;
    {
        nx::scoped_lock<nx::mutex> held(st.lock);
        Context* c = context_of(context);
        if (!c) return err::kInvalidPointer;
        if (!c->in_list) return err::kNotWithinCommandList;
        Context::Buffer& b = vertex ? c->vertex_buffer : c->fragment_buffer;
        if (b.address && b.used + size <= b.size) {
            const u32 at = b.address + b.used;
            b.used += size;
            (vertex ? c->vertex_default_uniforms : c->fragment_default_uniforms) = at;
            *out = at;
            return 0;
        }
        callback = vertex ? c->deferred_params.vertex_callback : c->deferred_params.fragment_callback;
        user = c->deferred_params.user_data;
        scratch = c->callback_out;
    }
    if (!callback || !scratch) return err::kReserveFailed;
    const u32 minimum = nx::max<u32>(size, 1024);  // (SCE_GXM_MINIMUM_DEFERRED_CONTEXT_BUFFER_SIZE)
    *reinterpret_cast<u32*>(memory().host(scratch)) = 0;
    const u32 args[] = {user, minimum, scratch};
    const u32 mem = runtime::hle::call_guest(ctx, callback, args);
    const u32 got = *reinterpret_cast<const u32*>(memory().host(scratch));
    if (!mem || got < size || !memory().valid(mem, got, true)) return err::kReserveFailed;
    nx::scoped_lock<nx::mutex> held(st.lock);
    Context* c = context_of(context);
    if (!c) return err::kInvalidPointer;
    (vertex ? c->vertex_buffer : c->fragment_buffer) = Context::Buffer{mem, got, size};
    (vertex ? c->vertex_default_uniforms : c->fragment_default_uniforms) = mem;
    *out = mem;
    return 0;
}

i32 reserve(PsvContext* ctx, u32 context, bool vertex, Ptr<u32> out) {
    if (!context || !out) return err::kInvalidPointer;
    u32 size = 0;
    {
        nx::scoped_lock<nx::mutex> held(state().lock);
        Context* c = context_of(context);
        if (!c) return err::kInvalidPointer;
        const Program* program = vertex ? (c->vertex ? c->vertex->program : nullptr) : (c->fragment ? c->fragment->program : nullptr);
        if (!program) return err::kNullProgram;
        size = (nx::max<u32>(program->gxp.default_uniform_buffer_size, 4) + 15) & ~15u;
        if (!c->deferred) {
            const u32 ring = vertex ? c->params.vertex_ring : c->params.fragment_ring;
            const u32 ring_size = vertex ? c->params.vertex_ring_size : c->params.fragment_ring_size;
            u32& used = vertex ? c->vertex_ring_used : c->fragment_ring_used;
            if (!ring || size > ring_size) return err::kReserveFailed;
            if (used + size > ring_size) used = 0;  // (draws copy their uniforms: the ring can wrap at once)
            const u32 at = ring + used;
            used += size;
            (vertex ? c->vertex_default_uniforms : c->fragment_default_uniforms) = at;
            *out = at;
            return 0;
        }
    }
    return reserve_deferred(ctx, context, vertex, size, out);
}
i32 sceGxmReserveVertexDefaultUniformBuffer(PsvContext* ctx, u32 context, Ptr<u32> out) { return reserve(ctx, context, true, out); }
i32 sceGxmReserveFragmentDefaultUniformBuffer(PsvContext* ctx, u32 context, Ptr<u32> out) { return reserve(ctx, context, false, out); }

i32 sceGxmSetVertexDefaultUniformBuffer(u32 context, u32 data) {
    if (!context || !data) return err::kInvalidPointer;
    with(context, [&](Context& c) { c.vertex_default_uniforms = data; });
    return 0;
}
i32 sceGxmSetFragmentDefaultUniformBuffer(u32 context, u32 data) {
    if (!context || !data) return err::kInvalidPointer;
    with(context, [&](Context& c) { c.fragment_default_uniforms = data; });
    return 0;
}
i32 sceGxmSetVertexUniformBuffer(u32 context, u32 index, u32 data) {
    if (!context || !data) return err::kInvalidPointer;
    if (index >= 14) return err::kInvalidValue;
    with(context, [&](Context& c) { c.vertex_buffers[index] = data; });
    return 0;
}
i32 sceGxmSetFragmentUniformBuffer(u32 context, u32 index, u32 data) {
    if (!context || !data) return err::kInvalidPointer;
    if (index >= 14) return err::kInvalidValue;
    with(context, [&](Context& c) { c.fragment_buffers[index] = data; });
    return 0;
}
i32 sceGxmSetFragmentTexture(u32 context, u32 unit, Ptr<const TextureWords> texture) {
    if (!context || !texture) return err::kInvalidPointer;
    if (unit >= 16) return err::kInvalidValue;
    with(context, [&](Context& c) { c.fragment_textures[unit] = *texture; });
    return 0;
}
i32 sceGxmSetVertexTexture(u32 context, u32 unit, Ptr<const TextureWords> texture) {
    if (!context || !texture) return err::kInvalidPointer;
    if (unit >= 4) return err::kInvalidValue;
    with(context, [&](Context& c) { c.vertex_textures[unit] = *texture; });
    return 0;
}

// Writes floats into a uniform buffer as a parameter lays them out: each element of its array holds its components
// in its type; vector elements of floating-point types start on 64-bit boundaries, others on 32-bit ones.
i32 sceGxmSetUniformDataF(u32 buffer, u32 parameter, u32 offset, u32 count, Ptr<const f32> data) {
    if (!buffer || !parameter || !data) return err::kInvalidPointer;
    if (!memory().valid(parameter, 16)) return err::kInvalidPointer;
    const u32* record = reinterpret_cast<const u32*>(memory().host(parameter));
    const u32 info = record[1];
    if ((info & 0xF) != 1) return err::kInvalidValue;  // (uniforms only)
    const u32 type = (info >> 4) & 0xF, components = (info >> 8) & 0xF;
    const u32 array = nx::max<u32>(record[2], 1);
    if (!components || offset + count > components * array) return err::kInvalidValue;
    u32 size;
    switch (type) {
        case 0: case 3: case 4: size = 4; break;  // F32, U32, S32
        case 1: case 5: case 6: size = 2; break;  // F16, U16, S16
        case 7: case 8: size = 1; break;          // U8, S8
        default: return err::kInvalidValue;       // (fixed point and aggregates)
    }
    const bool floating = type == 0 || type == 1;
    const u32 bytes = components * size;
    const u32 stride = floating && components > 1 ? (bytes + 7) & ~7u : (bytes + 3) & ~3u;
    const u32 base = buffer + record[3] * 4;
    if (!memory().valid(base, stride * array, true)) return err::kInvalidPointer;
    u8* out = memory().host(base);
    for (u32 i = 0; i < count; ++i) {
        const u32 k = offset + i;
        u8* at = out + (k / components) * stride + (k % components) * size;
        const f32 v = data.get()[i];
        switch (type) {
            case 0: std::memcpy(at, &v, 4); break;
            case 1: { const u16 h = to_half(v); std::memcpy(at, &h, 2); break; }
            case 3: { const u32 u = static_cast<u32>(nx::clamp(v, 0.0f, 4294967040.0f)); std::memcpy(at, &u, 4); break; }
            case 4: { const i32 s = static_cast<i32>(nx::clamp(v, -2147483648.0f, 2147483520.0f)); std::memcpy(at, &s, 4); break; }
            case 5: { const u16 u = static_cast<u16>(nx::clamp(v, 0.0f, 65535.0f)); std::memcpy(at, &u, 2); break; }
            case 6: { const i16 s = static_cast<i16>(nx::clamp(v, -32768.0f, 32767.0f)); std::memcpy(at, &s, 2); break; }
            case 7: *at = static_cast<u8>(nx::clamp(v, 0.0f, 255.0f)); break;
            case 8: *at = static_cast<u8>(static_cast<i8>(nx::clamp(v, -128.0f, 127.0f))); break;
        }
    }
    return 0;
}

// ---- Draws -------------------------------------------------------------------------------------------------------

nx::vector<u32> copy_words(u32 address, u32 bytes) {
    nx::vector<u32> words((bytes + 3) / 4, 0);
    if (address && bytes && memory().valid(address, bytes)) std::memcpy(words.data(), memory().host(address), bytes);
    return words;
}

// A stage's resources from a precomputed state: its default uniform buffer's words, buffers and textures.
template <typename StateT>
bool from_state(u32 address, u32 default_size, u32 textures, nx::vector<u32>& uniforms, u32* buffers, TextureWords* units) {
    if (!memory().valid(address, sizeof(StateT))) return false;
    const StateT& s = *reinterpret_cast<const StateT*>(memory().host(address));
    uniforms = copy_words(s.default_buffer, default_size);
    if (!memory().valid(s.block, 16 * 4 + textures * 16)) return true;
    std::memcpy(buffers, memory().host(s.block), 16 * 4);
    std::memcpy(units, memory().host(s.block + 16 * 4), textures * 16);
    return true;
}

// A draw with the context's state (streams: the precomputed draw's, else the context's).
i32 draw(u32 context, u32 primitive, u32 index_format, u32 indices, u32 count, u32 instances, const u32* streams = nullptr) {
    if (!context || !indices) return err::kInvalidPointer;
    State& st = state();
    nx::scoped_lock<nx::mutex> held(st.lock);
    Context* c = context_of(context);
    if (!c) return err::kInvalidPointer;
    if (c->deferred ? !c->in_list : !c->in_scene) return c->deferred ? err::kNotWithinCommandList : err::kNotWithinScene;
    if (!c->vertex || !c->fragment) return err::kNullProgram;
    if (!count) return err::kInvalidIndexCount;
    if (primitive > 0x14000000u || (primitive & 0x03FFFFFFu)) return err::kInvalidPrimitiveType;
    Draw d;
    d.vertex = c->vertex;
    d.fragment = c->fragment;
    d.state = c->state;
    d.primitive = primitive;
    d.index_format = index_format;
    d.index_address = indices;
    d.index_count = count;
    d.instances = instances;
    std::memcpy(d.streams, streams ? streams : c->streams, sizeof d.streams);
    const u32 vsize = d.vertex->program->gxp.default_uniform_buffer_size;
    const u32 fsize = d.fragment->program->gxp.default_uniform_buffer_size;
    if (vsize && !c->vertex_state && !c->vertex_default_uniforms) return err::kUniformBufferNotReserved;
    if (fsize && !c->fragment_state && !c->fragment_default_uniforms) return err::kUniformBufferNotReserved;
    if (!c->vertex_state || !from_state<PrecomputedVertexState>(c->vertex_state, vsize, 4, d.vertex_uniforms,
                                                                 d.vertex_buffers, d.vertex_textures)) {
        d.vertex_uniforms = copy_words(c->vertex_default_uniforms, vsize);
        std::memcpy(d.vertex_buffers, c->vertex_buffers, sizeof d.vertex_buffers);
        std::memcpy(d.vertex_textures, c->vertex_textures, sizeof d.vertex_textures);
    }
    if (!c->fragment_state || !from_state<PrecomputedFragmentState>(c->fragment_state, fsize, 16, d.fragment_uniforms,
                                                                     d.fragment_buffers, d.fragment_textures)) {
        d.fragment_uniforms = copy_words(c->fragment_default_uniforms, fsize);
        std::memcpy(d.fragment_buffers, c->fragment_buffers, sizeof d.fragment_buffers);
        std::memcpy(d.fragment_textures, c->fragment_textures, sizeof d.fragment_textures);
    }
    c->scene.draws.push_back(std::move(d));
    return 0;
}

i32 sceGxmDrawPrecomputed(u32 context, Ptr<const PrecomputedDraw> draw_object) {
    if (!draw_object) return err::kInvalidPointer;
    const PrecomputedDraw pd = *draw_object;
    u32 streams[16] = {};
    if (pd.block && memory().valid(pd.block, kPrecomputedDrawBlock)) std::memcpy(streams, memory().host(pd.block), sizeof streams);
    return draw(context, pd.primitive, pd.index_format, pd.index_data, pd.index_count, nx::max(pd.instances, 1u), streams);
}

// ---- Deferred contexts and command lists ----------------------------------------------------------------------

i32 sceGxmCreateDeferredContext(Ptr<const DeferredContextParams> p, Ptr<u32> out) {
    if (!p || !out) return err::kInvalidPointer;
    const DeferredContextParams params = *p;
    if (!params.host_mem || !memory().valid(params.host_mem, nx::max<u32>(params.host_mem_size, 4), true))
        return err::kInvalidValue;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    if (!s.initialized) return err::kUninitialized;
    auto c = nx::make_unique<Context>();
    c->guest = params.host_mem;
    c->deferred = true;
    c->deferred_params = params;
    c->vdm_buffer = {params.vdm_mem, params.vdm_mem_size, 0};
    c->vertex_buffer = {params.vertex_mem, params.vertex_mem_size, 0};
    c->fragment_buffer = {params.fragment_mem, params.fragment_mem_size, 0};
    c->callback_out = guest_alloc(4);
    s.contexts[c->guest] = std::move(c);
    *out = params.host_mem;
    return 0;
}

i32 sceGxmDestroyDeferredContext(u32 context) {
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Context* c = context_of(context);
    if (!c || !c->deferred) return err::kInvalidPointer;
    if (c->in_list) return err::kWithinCommandList;
    s.contexts.erase(context);
    return 0;
}

// The deferred context's buffers (VDM, vertex, fragment): set (NULL: the callbacks provide the next) and read back.
i32 set_deferred_buffer(u32 context, u32 which, u32 mem, u32 size) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    Context* c = context_of(context);
    if (!c) return err::kInvalidPointer;
    if (!c->deferred) return err::kInvalidValue;
    if (c->in_list) return err::kWithinCommandList;
    if (mem && size < 1024) return err::kInvalidValue;
    Context::Buffer& b = which == 0 ? c->vdm_buffer : which == 1 ? c->vertex_buffer : c->fragment_buffer;
    b = {mem, mem ? size : 0, 0};
    return 0;
}
i32 get_deferred_buffer(u32 context, u32 which, Ptr<u32> out) {
    if (!out) return err::kInvalidPointer;
    nx::scoped_lock<nx::mutex> held(state().lock);
    Context* c = context_of(context);
    if (!c) return err::kInvalidPointer;
    if (!c->deferred) return err::kInvalidValue;
    *out = (which == 0 ? c->vdm_buffer : which == 1 ? c->vertex_buffer : c->fragment_buffer).address;
    return 0;
}
i32 sceGxmSetDeferredContextVdmBuffer(u32 c, u32 mem, u32 size) { return set_deferred_buffer(c, 0, mem, size); }
i32 sceGxmSetDeferredContextVertexBuffer(u32 c, u32 mem, u32 size) { return set_deferred_buffer(c, 1, mem, size); }
i32 sceGxmSetDeferredContextFragmentBuffer(u32 c, u32 mem, u32 size) { return set_deferred_buffer(c, 2, mem, size); }
i32 sceGxmGetDeferredContextVdmBuffer(u32 c, Ptr<u32> out) { return get_deferred_buffer(c, 0, out); }
i32 sceGxmGetDeferredContextVertexBuffer(u32 c, Ptr<u32> out) { return get_deferred_buffer(c, 1, out); }
i32 sceGxmGetDeferredContextFragmentBuffer(u32 c, Ptr<u32> out) { return get_deferred_buffer(c, 2, out); }

i32 sceGxmBeginCommandList(u32 context) {
    nx::scoped_lock<nx::mutex> held(state().lock);
    Context* c = context_of(context);
    if (!c) return err::kInvalidPointer;
    if (!c->deferred) return err::kInvalidValue;
    if (c->in_list) return err::kWithinCommandList;
    c->scene = Scene{};
    c->in_list = true;
    return 0;
}

// The list's draws are kept under an id in the SceGxmCommandList (a list ended again into the same object frees
// the draws it named).
i32 sceGxmEndCommandList(u32 context, Ptr<u32> list) {
    if (!list) return err::kInvalidPointer;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Context* c = context_of(context);
    if (!c) return err::kInvalidPointer;
    if (!c->deferred) return err::kInvalidValue;
    if (!c->in_list) return err::kNotWithinCommandList;
    if (list.get()[0] == kCommandListTag) s.command_lists.erase(list.get()[1]);
    const u32 id = s.next_command_list++;
    s.command_lists[id] = std::move(c->scene.draws);
    c->scene = Scene{};
    c->in_list = false;
    c->vertex_default_uniforms = c->fragment_default_uniforms = 0;  // (reservations end with the list)
    u32* words = list.get();
    std::memset(words, 0, 8 * 4);
    words[0] = kCommandListTag;
    words[1] = id;
    return 0;
}

// The list's draws join the scene (each carries its own state: the immediate context's is unchanged).
i32 sceGxmExecuteCommandList(u32 context, Ptr<const u32> list) {
    if (!list) return err::kInvalidPointer;
    State& s = state();
    nx::scoped_lock<nx::mutex> held(s.lock);
    Context* c = context_of(context);
    if (!c) return err::kInvalidPointer;
    if (c->deferred) return err::kInvalidValue;
    if (!c->in_scene) return err::kNotWithinScene;
    if (list.get()[0] != kCommandListTag) return err::kInvalidValue;
    auto it = s.command_lists.find(list.get()[1]);
    if (it == s.command_lists.end()) return err::kInvalidValue;
    for (const Draw& d : it->second) c->scene.draws.push_back(d);
    return 0;
}

void sceGxmSetPrecomputedVertexState(u32 context, u32 precomputed) {
    with(context, [&](Context& c) { c.vertex_state = precomputed; });
}
void sceGxmSetPrecomputedFragmentState(u32 context, u32 precomputed) {
    with(context, [&](Context& c) { c.fragment_state = precomputed; });
}

i32 sceGxmDraw(u32 context, u32 primitive, u32 index_format, u32 indices, u32 count) {
    return draw(context, primitive, index_format, indices, count, 1);
}
i32 sceGxmDrawInstanced(u32 context, u32 primitive, u32 index_format, u32 indices, u32 count, u32 wrap) {
    if (!wrap || count % wrap) return err::kInvalidValue;
    return draw(context, primitive, index_format, indices, wrap, count / wrap);
}

const runtime::hle::Function kFunctions[] = {
    HLE_FUNCTION(0x8734FF4E, sceGxmBeginScene),
    HLE_FUNCTION(0xDBA33160, sceGxmBeginScene),  // (_sceGxmBeginScene)
    HLE_FUNCTION(0x4709CF5A, sceGxmBeginSceneEx),
    HLE_FUNCTION(0xFE300E2F, sceGxmEndScene),
    HLE_FUNCTION(0x2B5C0444, sceGxmMidSceneFlush),
    HLE_FUNCTION(0x51FE0899, sceGxmMidSceneFlush),  // (_sceGxmMidSceneFlush)
    HLE_FUNCTION(0x31FF8ABD, sceGxmSetVertexProgram),
    HLE_FUNCTION(0xAD2F48D9, sceGxmSetFragmentProgram),
    HLE_FUNCTION(0x895DF2E9, sceGxmSetVertexStream),
    HLE_FUNCTION(0x14BD831F, sceGxmSetFrontDepthFunc),
    HLE_FUNCTION(0xB042A4D2, sceGxmSetBackDepthFunc),
    HLE_FUNCTION(0xF32CBF34, sceGxmSetFrontDepthWriteEnable),
    HLE_FUNCTION(0xC18B706B, sceGxmSetBackDepthWriteEnable),
    HLE_FUNCTION(0xB8645A9A, sceGxmSetFrontStencilFunc),
    HLE_FUNCTION(0x1A68C8D2, sceGxmSetBackStencilFunc),
    HLE_FUNCTION(0x8FA6FE44, sceGxmSetFrontStencilRef),
    HLE_FUNCTION(0x866A0517, sceGxmSetBackStencilRef),
    HLE_FUNCTION(0xAAA97F81, sceGxmSetFrontDepthBias),
    HLE_FUNCTION(0x17B3BF86, sceGxmSetBackDepthBias),
    HLE_FUNCTION(0xFD93209D, sceGxmSetFrontPolygonMode),
    HLE_FUNCTION(0xF66EC6FE, sceGxmSetBackPolygonMode),
    HLE_FUNCTION(0x06752183, sceGxmSetFrontPointLineWidth),
    HLE_FUNCTION(0x8DCB0EDB, sceGxmSetBackPointLineWidth),
    HLE_FUNCTION(0x575958A8, sceGxmSetFrontFragmentProgramEnable),
    HLE_FUNCTION(0xE26B4834, sceGxmSetBackFragmentProgramEnable),
    HLE_FUNCTION(0x5765DE9F, sceGxmSetFrontLineFillLastPixelEnable),
    HLE_FUNCTION(0xC88EB702, sceGxmSetBackLineFillLastPixelEnable),
    HLE_FUNCTION(0xE1CA72AE, sceGxmSetCullMode),
    HLE_FUNCTION(0x0DE9AEB7, sceGxmSetTwoSidedEnable),
    HLE_FUNCTION(0x30459117, sceGxmSetFrontVisibilityTestEnable),
    HLE_FUNCTION(0x17CF46B9, sceGxmSetBackVisibilityTestEnable),
    HLE_FUNCTION(0x12625C34, sceGxmSetFrontVisibilityTestIndex),
    HLE_FUNCTION(0xAE7886FE, sceGxmSetBackVisibilityTestIndex),
    HLE_FUNCTION(0xD0E3CD9A, sceGxmSetFrontVisibilityTestOp),
    HLE_FUNCTION(0xC83F0AB3, sceGxmSetBackVisibilityTestOp),
    HLE_FUNCTION(0x7767EC49, sceGxmSetVisibilityBuffer),
    HLE_FUNCTION(0x70C86868, sceGxmSetRegionClip),
    HLE_FUNCTION(0x60CF708E, sceGxmSetDefaultRegionClipAndViewport),
    HLE_FUNCTION(0x3EB3380B, sceGxmSetViewport),
    HLE_FUNCTION(0x814F61EB, sceGxmSetViewportEnable),
    HLE_FUNCTION(0xD096336E, sceGxmSetWClampValue),
    HLE_FUNCTION(0x1BF8B853, sceGxmSetWClampEnable),
    HLE_FUNCTION(0xEED86975, sceGxmSetWBufferEnable),
    HLE_FUNCTION(0xB32917F0, sceGxmSetYuvProfile),
    HLE_FUNCTION(0xC7A8CB77, sceGxmSetUserMarker),
    HLE_FUNCTION(0x8C6A24C9, sceGxmSetValidationEnable),
    HLE_FUNCTION(0x97118913, sceGxmReserveVertexDefaultUniformBuffer),
    HLE_FUNCTION(0x7B1FABB6, sceGxmReserveFragmentDefaultUniformBuffer),
    HLE_FUNCTION(0xC697CAE5, sceGxmSetVertexDefaultUniformBuffer),
    HLE_FUNCTION(0xA824EB24, sceGxmSetFragmentDefaultUniformBuffer),
    HLE_FUNCTION(0xC68015E4, sceGxmSetVertexUniformBuffer),
    HLE_FUNCTION(0xEA0FC310, sceGxmSetFragmentUniformBuffer),
    HLE_FUNCTION(0x29C34DF5, sceGxmSetFragmentTexture),
    HLE_FUNCTION(0x9EB4380F, sceGxmSetVertexTexture),
    HLE_FUNCTION(0x16C9D339, sceGxmSetVertexTexture),  // (_sceGxmSetVertexTexture)
    HLE_FUNCTION(0x65DD0C84, sceGxmSetUniformDataF),
    HLE_FUNCTION(0xBC059AFC, sceGxmDraw),
    HLE_FUNCTION(0xED3F78B8, sceGxmDrawPrecomputed),
    HLE_FUNCTION(0x249D5B00, sceGxmCreateDeferredContext),
    HLE_FUNCTION(0xD6A2FF2F, sceGxmDestroyDeferredContext),
    HLE_FUNCTION(0x0B9D13CE, sceGxmSetDeferredContextVdmBuffer),
    HLE_FUNCTION(0x27CAD127, sceGxmSetDeferredContextVertexBuffer),
    HLE_FUNCTION(0xC6B3FCD0, sceGxmSetDeferredContextFragmentBuffer),
    HLE_FUNCTION(0xAD8C2EBA, sceGxmGetDeferredContextVdmBuffer),
    HLE_FUNCTION(0x87632B9C, sceGxmGetDeferredContextVertexBuffer),
    HLE_FUNCTION(0xFA081D05, sceGxmGetDeferredContextFragmentBuffer),
    HLE_FUNCTION(0x944D3F83, sceGxmBeginCommandList),
    HLE_FUNCTION(0x36D85916, sceGxmEndCommandList),
    HLE_FUNCTION(0xE8E139DD, sceGxmExecuteCommandList),
    HLE_FUNCTION(0xB7626A93, sceGxmSetPrecomputedVertexState),
    HLE_FUNCTION(0xF8952750, sceGxmSetPrecomputedFragmentState),
    HLE_FUNCTION(0x14C4E7D3, sceGxmDrawInstanced),
};

}  // namespace

void notify(Ptr<const Notification> n) {
    if (n && memory().valid(n->address, 4, true)) *reinterpret_cast<u32*>(memory().host(n->address)) = n->value;
}

void register_context() { runtime::hle::register_library({"SceGxm", kSceGxm, kFunctions}); }

}  // namespace psvrecore::gxm::hle
