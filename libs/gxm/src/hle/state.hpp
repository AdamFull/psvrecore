#pragma once

// The GXM layer's state: guest structures (laid out as the SDK's headers define them), the host objects behind the
// handles GXM gives the game (each handle is the guest address of a small block, so the game can keep and compare
// it), and the global state of the library.

#include <atomic>

#include "psvrecore/gxm/hle.hpp"
#include "psvrecore/runtime/hle.hpp"
#include "psvrecore/runtime/runtime.hpp"
#include "psvrecore/usse/decode.hpp"
#include "psvrecore/usse/gxp.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/threading/sync.h"
#include "core/foundation/threading/thread.h"

namespace psvrecore::gxm::hle {

using runtime::hle::Ptr;

// Error codes (SCE_GXM_ERROR_*).
namespace err {
inline constexpr i32 code(u32 c) { return static_cast<i32>(c); }
inline constexpr i32 kUninitialized = code(0x805B0000), kAlreadyInitialized = code(0x805B0001),
                     kOutOfMemory = code(0x805B0002), kInvalidValue = code(0x805B0003),
                     kInvalidPointer = code(0x805B0004), kInvalidAlignment = code(0x805B0005),
                     kNotWithinScene = code(0x805B0006), kWithinScene = code(0x805B0007),
                     kNullProgram = code(0x805B0008), kUnsupported = code(0x805B0009),
                     kPatcherInternal = code(0x805B000A), kReserveFailed = code(0x805B000B),
                     kProgramInUse = code(0x805B000C), kInvalidIndexCount = code(0x805B000D),
                     kUniformBufferNotReserved = code(0x805B0011), kDriver = code(0x805B0017),
                     kInvalidTexture = code(0x805B0020), kOutOfHostMemory = code(0x805B0021),
                     kInvalidPrimitiveType = code(0x805B0025), kInvalidMapping = code(0x805B0026),
                     kOutOfRenderTargets = code(0x805B0027);
}  // namespace err

// ---- Guest structures --------------------------------------------------------------------------------------------

struct InitializeParams {
    u32 flags, display_queue_max_pending, display_queue_callback, display_queue_callback_data_size,
        parameter_buffer_size;
};

struct ContextParams {
    u32 host_mem, host_mem_size, vdm_ring, vdm_ring_size, vertex_ring, vertex_ring_size, fragment_ring,
        fragment_ring_size, fragment_usse_ring, fragment_usse_ring_size, fragment_usse_ring_offset;
};

struct RenderTargetParams {
    u32 flags;
    u16 width, height, scenes_per_frame, multisample_mode;
    u32 multisample_locations;
    i32 driver_mem_block;
};
static_assert(sizeof(RenderTargetParams) == 20);

struct PatcherParams {
    u32 user_data, host_alloc, host_free, buffer_alloc, buffer_free, buffer_mem, buffer_mem_size, vertex_usse_alloc,
        vertex_usse_free, vertex_usse_mem, vertex_usse_mem_size, vertex_usse_offset, fragment_usse_alloc,
        fragment_usse_free, fragment_usse_mem, fragment_usse_mem_size, fragment_usse_offset;
};

struct VertexAttribute {
    u16 stream, offset;
    u8 format, components;
    u16 reg;
};
static_assert(sizeof(VertexAttribute) == 8);

struct VertexStream {
    u16 stride, index_source;
};

// Blending: a colour mask, then 4-bit fields from the low bits of each byte (colour function, alpha function;
// colour source, colour destination; alpha source, alpha destination).
struct BlendInfo {
    u8 color_mask;
    u8 funcs;
    u8 color_factors;
    u8 alpha_factors;
    u32 color_func() const { return funcs & 15; }
    u32 alpha_func() const { return funcs >> 4; }
    u32 color_src() const { return color_factors & 15; }
    u32 color_dst() const { return color_factors >> 4; }
    u32 alpha_src() const { return alpha_factors & 15; }
    u32 alpha_dst() const { return alpha_factors >> 4; }
};
static_assert(sizeof(BlendInfo) == 4);

// The opaque words of a colour surface hold this layer's own encoding (games only use them through the API).
struct ColorSurface {
    u32 format;         // (the "sideband" word) SceGxmColorFormat
    u32 data;           // emit words: the data address,
    u32 size;           //   width | height << 16,
    u32 stride;         //   the stride in pixels,
    u32 modes;          //   type | scale mode << 4 | gamma << 8 | dither << 12,
    u32 clip_min;       //   the clip rectangle's first pixel x | y << 16
    u32 clip_max;       //   and last pixel x | y << 16
    u32 output_register_size;
    u32 background[4];  // (unused)
};
static_assert(sizeof(ColorSurface) == 48);

struct DepthStencilSurface {
    u32 control;  // format | kDs* bits (see surfaces.cpp)
    u32 depth_data, stencil_data;
    f32 background_depth;
    u32 background_control;  // stencil | mask bit << 8 (see surfaces.cpp)
};
static_assert(sizeof(DepthStencilSurface) == 20);
// DepthStencilSurface::control bits besides the format.
inline constexpr u32 kDsTiled = 1u << 28, kDsForceLoad = 1u << 29, kDsForceStore = 1u << 30, kDsEnabled = 1u << 31;

struct Notification {
    u32 address, value;
};

struct ValidRegion {
    u32 x_max, y_max;
};

struct TextureWords {
    u32 words[4];
};

struct DisplayFrameBuf {
    u32 size, base, pitch, pixel_format, width, height;
};

// ---- Host objects ------------------------------------------------------------------------------------------------

struct SyncObject {
    u32 guest = 0;
};

struct RenderTarget {
    u32 guest = 0;
    u32 width = 0, height = 0, scenes_per_frame = 0, multisample = 0;
};

// A program the patcher registered: its GXP (parsed and decoded once).
struct Program {
    u32 guest = 0;  // the SceGxmShaderPatcherId
    u32 gxp_address = 0;
    usse::Gxp gxp;
    nx::vector<usse::Decoded> primary, secondary;
    u32 references = 1;
};

struct VertexProgram {
    u32 guest = 0;
    Program* program = nullptr;
    nx::vector<VertexAttribute> attributes;
    nx::vector<VertexStream> streams;
    u32 references = 1;
};

struct FragmentProgram {
    u32 guest = 0;
    Program* program = nullptr;
    u32 output_format = 0;
    u32 multisample = 0;
    bool blend_enabled = false;
    BlendInfo blend{};
    u32 vertex_gxp = 0;  // (the vertex program it was made for; varyings are matched to it)
    u32 references = 1;
    // A mask update program (sceGxmShaderPatcherCreateMaskUpdateFragmentProgram): its draws set or clear the mask
    // bit of a DF32M surface; `program` is an empty program of the driver's (State::mask_update_program).
    bool mask_update = false;
};

struct Patcher {
    u32 guest = 0;
    PatcherParams params{};
    nx::vector<Program*> programs;
    nx::vector<VertexProgram*> vertex_programs;
    nx::vector<FragmentProgram*> fragment_programs;
};

// Fixed-function state that draws take, as GXM's enumerations give it (front faces, then back faces where GXM has
// two). The initial values are a new context's (the reference lists them).
struct StencilState {
    u32 func = 0x0E000000;  // SCE_GXM_STENCIL_FUNC_ALWAYS
    u32 fail = 0, depth_fail = 0, pass = 0;  // SCE_GXM_STENCIL_OP_KEEP
    u8 compare_mask = 0, write_mask = 0;
    u8 ref = 0;
};

struct DrawState {
    u32 depth_func[2] = {0x00C00000, 0x00C00000};  // SCE_GXM_DEPTH_FUNC_LESS_EQUAL
    bool depth_write[2] = {true, true};
    StencilState stencil[2];
    u32 cull = 0;  // SCE_GXM_CULL_NONE
    bool two_sided = false;
    u32 polygon_mode[2] = {0, 0};  // SCE_GXM_POLYGON_MODE_TRIANGLE_FILL
    u32 point_line_width[2] = {1, 1};
    i32 depth_bias_factor[2] = {0, 0}, depth_bias_units[2] = {0, 0};
    bool fragment_enabled[2] = {true, true};
    bool viewport_enabled = true;
    f32 viewport[6] = {};  // x offset, x scale, y offset, y scale, z offset, z scale
    u32 region_clip_mode = 0;  // SCE_GXM_REGION_CLIP_NONE
    u32 region_min[2] = {0, 0}, region_max[2] = {0, 0};
    bool w_clamp_enabled = true;
    f32 w_clamp = 0.00001f;
    bool w_buffer = false;
};

// A draw as recorded in a scene: everything it needs, copied when it was made (uniform data in particular: the
// game may reuse its ring buffer memory before the scene is rendered).
struct Draw {
    VertexProgram* vertex = nullptr;
    FragmentProgram* fragment = nullptr;
    DrawState state;
    u32 primitive = 0, index_format = 0, index_address = 0, index_count = 0, instances = 1;
    u32 streams[16] = {};
    nx::vector<u32> vertex_uniforms, fragment_uniforms;  // the default uniform buffers' words
    u32 vertex_buffers[16] = {}, fragment_buffers[16] = {};
    TextureWords fragment_textures[16] = {}, vertex_textures[4] = {};
};

struct Scene {
    RenderTarget* target = nullptr;
    ColorSurface color{};
    bool has_color = false;
    DepthStencilSurface depth{};
    bool has_depth = false;
    ValidRegion region{};
    u32 flags = 0;
    nx::vector<Draw> draws;
};

struct Context {
    u32 guest = 0;
    ContextParams params{};
    bool in_scene = false;
    Scene scene;
    DrawState state;
    VertexProgram* vertex = nullptr;
    FragmentProgram* fragment = nullptr;
    u32 streams[16] = {};
    u32 vertex_default_uniforms = 0, fragment_default_uniforms = 0;  // reserved this draw (guest addresses)
    u32 vertex_buffers[16] = {}, fragment_buffers[16] = {};
    TextureWords fragment_textures[16] = {}, vertex_textures[4] = {};
    u32 vertex_ring_used = 0, fragment_ring_used = 0;  // (bump offsets, wrapping)
};

struct DisplayEntry {
    u32 old_buffer = 0, new_buffer = 0;
    u32 data = 0;  // a copy of the callback data, in guest memory
};

// The library's state (one per process).
struct State {
    const Options* options = nullptr;
    nx::mutex lock;  // (the GXM objects; draws are made by one thread per context)
    bool initialized = false;
    InitializeParams init{};
    u32 notification_region = 0;

    nx::hash_map<u32, nx::unique_ptr<SyncObject>> sync_objects;
    nx::hash_map<u32, nx::unique_ptr<RenderTarget>> render_targets;
    nx::hash_map<u32, nx::unique_ptr<Context>> contexts;
    nx::hash_map<u32, nx::unique_ptr<Patcher>> patchers;
    nx::hash_map<u32, nx::unique_ptr<Program>> programs;
    nx::hash_map<u32, nx::unique_ptr<VertexProgram>> vertex_programs;
    nx::hash_map<u32, nx::unique_ptr<FragmentProgram>> fragment_programs;
    nx::unique_ptr<Program> mask_update_program;  // (made on first use)
    nx::vector<std::pair<u32, u32>> mappings;  // (base, size) mapped for the GPU

    // The display queue: entries waiting for the display thread, which calls the game's callback for each.
    nx::mutex queue_lock;
    nx::cond_var queue_changed;
    nx::vector<DisplayEntry> queue;
    u32 queue_busy = 0;  // (an entry being shown)
    nx::vector<u32> queue_data_free;  // guest blocks for callback data
    nx::thread display_thread;
    bool stopping = false;

    // Small guest blocks for handles and copies.
    u32 arena_next = 0, arena_end = 0;
};

State& state();
// A guest block of `size` bytes (16-byte aligned) for the library's own use; 0 if guest memory is exhausted.
u32 guest_alloc(u32 size);

// Renders a scene (render.cpp) and writes its colour surface to guest memory.
void render_scene(Context& context, Scene& scene);

// The display (display.cpp): what the display queue thread needs.
void start_display_queue();
void stop_display_queue();

void register_display();
void register_core();
void register_surfaces();
void register_patcher();
void register_context();
void register_textures();
void register_transfer();

// Writes a notification's value to its address (if any).
void notify(Ptr<const Notification> n);

}  // namespace psvrecore::gxm::hle
