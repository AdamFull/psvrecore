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

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/usse/decode.hpp"
#include "psvrecore/usse/gxp.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::gxm {

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
};

Result<nx::vector<u32>> translate_secondary(const usse::Gxp& gxp, std::span<const usse::Decoded> program);
Result<nx::vector<u32>> translate_vertex(const usse::Gxp& gxp, std::span<const usse::Decoded> program,
                                         const VertexShader& shader);
// `secondary`: the program's secondary program, to run in each instance first (where it reads per-tile special
// registers, which a secondary program run once per draw cannot have).
Result<nx::vector<u32>> translate_fragment(const usse::Gxp& gxp, std::span<const usse::Decoded> program,
                                           const FragmentShader& shader, std::span<const usse::Decoded> secondary = {});

// The secondary attributes as the driver fills them before a draw: the default uniform buffer (its first words),
// the uniform buffers' addresses (by BUFFER index; index 14 the default buffer's copy in memory) and the literals.
// Sampler state words are left zero (the draw forms find texture units in the program's sampler slots).
void fill_secondary(const usse::Gxp& gxp, std::span<const u32> default_uniforms, std::span<const u32> buffer_addresses,
                    std::span<u32, 256> out);

}  // namespace psvrecore::gxm
