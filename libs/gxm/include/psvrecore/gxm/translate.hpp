#pragma once

// Translation of USSE programs to SPIR-V. A program's registers become function-scope arrays of 32-bit words and
// each instruction the same operations on them as the reference interpreter (usse/interp.hpp) performs; branches
// become a loop around a switch over the program's blocks.
//
// Four forms:
// - compute (the translator's test form): one primary program instance per invocation, on storage buffers: the
//   instance's primary attributes in, the secondary attributes shared, the primary attributes, output registers
//   and whether the instance was discarded out. Textures and guest memory are storage buffers too, in the test
//   model of psvusse --run (RGBA8 images sampled at the nearest texel with wrapping), so that the GPU's results can
//   be compared with the interpreter's exactly where the arithmetic allows;
// - secondary: a secondary program, one invocation per draw, on that draw's secondary attributes in a storage
//   buffer (it reads them as the driver set them up and leaves its results there for the draw's shaders);
// - vertex and fragment: a draw's shaders. The vertex shader takes the patcher's vertex attributes, runs the
//   primary program and transforms its position as GXM's viewport does; the fragment shader takes the varyings
//   (and the samples taken with them) as the program's input records place them, runs the program and writes its
//   output register as a colour.

#include <optional>
#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/usse/decode.hpp"
#include "psvrecore/usse/gxp.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::gxm {

// ---- Half precision ----------------------------------------------------------------------------------------------

// How translated programs round results to half precision (F16 registers) as the interpreter does (to nearest even,
// subnormals kept, a NaN's top mantissa bits kept and quieted): in integer arithmetic (about 28 instructions each),
// or with the device's own conversion (GLSL.std.450 PackHalf2x16: 2), where check_half_conversion found it gives
// the same bits; NaNs then still as the interpreter's where the device's differ (DeviceNaN: 12). A fragment program
// rounds a hundred results or more, and a phone's driver compiles each.
enum class HalfConversion : u8 { Integer, Device, DeviceNaN };
// (process-wide, set before translating: the shader cache keeps what one way translated apart from the others)
void set_half_conversion(HalfConversion c);
HalfConversion half_conversion();
const char* half_conversion_name(HalfConversion c);

// The check: a compute shader (local size 64 x 1 x 1) converting float32 values both ways and counting where they
// differ. Value i (from the push constant's first word plus the invocation's x, plus its y times 2^18) is, below
// kHalfCheckValues: each of 2^30 with exponents 96-159 (every value whose rounding matters, with both signs), the
// 2^24 infinities and NaNs, then 2^21 more over all exponents. In the storage buffer (set 0, binding 0): word 0 set
// to 1 where a value differs (not a NaN), word 1 where a NaN does, word 2 one such value (not a NaN).
inline constexpr u64 kHalfCheckValues = (1ull << 30) + (1ull << 24) + (1ull << 21);
nx::vector<u32> half_check_shader();

// ---- The compute form ----------------------------------------------------------------------------------------------

// Its buffers, in 32-bit words.
struct ComputeLayout {
    static constexpr u32 kPrimary = 64;   // per invocation, in: primary attributes 0-63; out: the same, after the run
    static constexpr u32 kOutput = 64;    // per invocation, out: output registers 0-63
    static constexpr u32 kSecondary = 256;  // shared: the secondary attributes
    static constexpr u32 kIn = kPrimary;
    static constexpr u32 kOut = kPrimary + kOutput + 1;  // (+ 1: non-zero when discarded)
    // Textures: kTextures headers of four words (width, height, the word index of the first texel, 0) by the unit
    // a sampler's first state word names, then the texels (RGBA8, rows from the top; cube faces stacked).
    static constexpr u32 kTextures = 16;
    // Memory: the guest address of word 2, the number of words, then the words (and two words of padding).
    static constexpr u32 kMemoryHeader = 2;
    static constexpr u32 kLocalSize = 1;
};

// Bindings of the compute form (descriptor set 0, all storage buffers).
inline constexpr u32 kBindingIn = 0, kBindingSecondary = 1, kBindingOut = 2, kBindingTextures = 3, kBindingMemory = 4;

// Translates a primary program to the compute form. Fails on an instruction the translator does not implement.
Result<nx::vector<u32>> translate_compute(const usse::Gxp& gxp, std::span<const usse::Decoded> program);

// ---- The draw forms (secondary, vertex, fragment) ------------------------------------------------------------------

// Descriptor set 0 of a draw's pipelines (and of the secondary programs' dispatches):
// - the secondary attributes: 512 words per draw (the vertex program's 256, then the fragment program's);
// - guest memory the programs read (uniform buffers), as the compute form's memory buffer;
// - combined image samplers: the fragment program's units 0-15, the vertex program's units 0-3.
// - the colour attachment as an input attachment (fragment programs that read the colour, FRAGCOLOR).
inline constexpr u32 kDrawBindingSecondary = 0, kDrawBindingMemory = 1, kDrawBindingFragmentTextures = 2,
                     kDrawBindingVertexTextures = 18, kDrawBindingColor = 22, kDrawBindings = 23;
inline constexpr u32 kDrawSecondaryWords = 512;

// The push constants of a draw (both stages, and the secondary dispatches).
struct DrawConstants {
    u32 draw = 0;   // the draw's index: its secondary attributes
    u32 flags = 0;  // kDrawViewport | kDrawPoints
    f32 target[2] = {};    // the render target's width and height
    f32 viewport[6] = {};  // x offset, x scale, y offset, y scale, z offset, z scale
    f32 w_clamp = 0.0f;    // (0: no clamp)
    u32 stage = 0;         // secondary dispatches: 0 the vertex program's, 1 the fragment program's
    // The internal resolution: pixels drawn per pixel of the render target. Positions are mapped through `target`
    // whatever it is; the fragment form's pixel coordinates are divided by it, and point sizes multiplied.
    f32 scale = 1.0f;
};
static_assert(sizeof(DrawConstants) == 52);
inline constexpr u32 kDrawViewport = 1, kDrawPoints = 2;

// Varying locations between the stages.
inline constexpr u32 kVaryingColor0 = 0, kVaryingColor1 = 1, kVaryingFog = 2, kVaryingTexCoord0 = 3;

// A vertex attribute as the patcher gives it (SceGxmVertexAttribute): its Vulkan input is at `location`, read as
// (vertex_input_read): floats (the normalized formats, F16, F32), unsigned or signed integers converted to floats
// in the shader (U8, U16, S8, S16), or unsigned integers holding the data's bits (raw: untyped attributes, and
// attributes the program reads in their declared format). The input may fetch more components than the attribute
// has (a 4-component format for a 3-component one): the shader takes the missing ones as 0, 0, 0, 1.
struct VertexInput {
    u32 location = 0;
    u32 reg = 0;         // the first primary attribute register
    u8 format = 0;       // SceGxmAttributeFormat
    u8 components = 0;   // (untyped: 32-bit words)
    bool raw = false;
};

enum class VertexRead : u8 { Float, Unsigned, Signed, Raw };
VertexRead vertex_input_read(const VertexInput& in);
// Bits per component of an attribute format (raw inputs pack them in this size).
u32 vertex_input_bits(u8 format);

struct VertexShader {
    nx::vector<VertexInput> inputs;
};

struct FragmentShader {
    // SceGxmOutputRegisterFormat the program was made for (the colour is read in the program's declared output type:
    // the attachment quantizes it to this format).
    u32 output_format = 0;
    // The colour's components from the output register's lanes (0-3: x, y, z, w; 4: zero, 5: one), in the
    // attachment's red, green, blue, alpha order.
    u8 swizzle[4] = {0, 1, 2, 3};
    bool fragment_program_enabled = true;
    // Whether the pass has a colour attachment (depth-only scenes: no colour output).
    bool color_output = true;
    // Programs that read the colour (FRAGCOLOR, with __nativecolor) find it in their output register before they
    // run: from the colour attachment (input attachment kDrawBindingColor), in the output register's format.
    // With `blend_read`, a program whose colour read color_read_blend finds leaves that blend to the pipeline's
    // fixed-function blending: its blend writes its other source to the output register, and nothing is read.
    bool blend_read = false;
    // The colour surface's gamma mode (sceGxmColorSurfaceSetGammaMode): the output register's lanes (bit k: lane k, a
    // colour lane, never the alpha one: 0 to 2) whose components the surface gamma-corrects as it writes them, linear
    // to sRGB, and decodes where a program reads the colour back (0: none). Lanes, not the attachment's channels: the
    // swizzle takes them there. (Fixed-function blending, which runs on the encoded values, is not the surface's: it
    // blends in encoded space.)
    u8 gamma = 0;
};

// A program's read of the colour that fixed-function blending can do: the program reads it (FRAGCOLOR, with
// __nativecolor, 8-bit lanes) only in its last write of the output register, a sum of products (sop2, blend) of the
// colour and another register with factors the pipeline's blending has. Its factors and operations, numbered as
// VkBlendFactor and VkBlendOp, for the output register's lanes; `lanes` the lanes it writes (the others keep the
// colour). Without rasterization-order attachment access, the fragments of one draw that read the colour run in no
// order: where its triangles overlap, they lose each other's writes (a light cone of layers drawn in one draw);
// blending is ordered.
struct ColorReadBlend {
    enum Factor : u8 {
        kZero = 0, kOne = 1, kSrcColor = 2, kOneMinusSrcColor = 3, kDstColor = 4, kOneMinusDstColor = 5,
        kSrcAlpha = 6, kOneMinusSrcAlpha = 7, kDstAlpha = 8, kOneMinusDstAlpha = 9, kSrcAlphaSaturate = 14,
    };
    enum Operation : u8 { kAdd = 0, kSubtract = 1, kReverseSubtract = 2, kMin = 3, kMax = 4 };
    u8 color_src = kOne, color_dst = kZero, alpha_src = kOne, alpha_dst = kZero;
    u8 color_op = kAdd, alpha_op = kAdd;
    u8 lanes = 0;
};
std::optional<ColorReadBlend> color_read_blend(const usse::Gxp& gxp, std::span<const usse::Decoded> program);

Result<nx::vector<u32>> translate_secondary(const usse::Gxp& gxp, std::span<const usse::Decoded> program);
Result<nx::vector<u32>> translate_vertex(const usse::Gxp& gxp, std::span<const usse::Decoded> program,
                                         const VertexShader& shader);
// `secondary`: the program's secondary program, to run in each instance first (where it reads per-tile special
// registers, which a secondary program run once per draw cannot have).
Result<nx::vector<u32>> translate_fragment(const usse::Gxp& gxp, std::span<const usse::Decoded> program,
                                           const FragmentShader& shader, std::span<const usse::Decoded> secondary = {});

// The secondary attributes as the driver fills them before a draw: the default uniform buffer (its first words),
// the first words of the user uniform buffers the program keeps in registers (`buffer_words`, by BUFFER index: what
// is known of each buffer's words; zeros past them), the uniform buffers' addresses (by BUFFER index; index 14 the
// default buffer's copy in memory) and the literals. Sampler state words are left zero (the draw forms find texture
// units in the program's sampler slots).
void fill_secondary(const usse::Gxp& gxp, std::span<const u32> default_uniforms, std::span<const u32> buffer_addresses,
                    std::span<u32, 256> out, std::span<const std::span<const u32>> buffer_words = {});

}  // namespace psvrecore::gxm
