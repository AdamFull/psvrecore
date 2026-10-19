#pragma once

// GXP files: compiled shader programs. A vertex or fragment program holds a primary program, run per vertex or
// fragment, a secondary one, run once per draw, and tables describing its interface: the parameters (attributes,
// uniforms, samplers, uniform buffers), the vertex outputs or fragment inputs, and flags. What is read here is
// what libgxm's program functions report (sceGxmProgram*), checked against the host build of those functions in
// the SDK (scripts/gxp_check.py).

#include <optional>
#include <span>

#include "psvrecore/error.hpp"
#include "core/foundation/core/foundation.h"

namespace psvrecore::usse {

enum class ShaderType : u8 { Vertex, Fragment };

// The values are libgxm's (SceGxmParameterCategory, SceGxmParameterType, SceGxmParameterSemantic).
enum class ParameterCategory : u8 { Attribute = 0, Uniform = 1, Sampler = 2, UniformBuffer = 4 };
enum class ParameterType : u8 { F32, F16, C10, U32, S32, U16, S16, U8, S8, Aggregate };
enum class Semantic : u8 {
    None, Attr, Bcol, Binormal, BlendIndices, BlendWeight, Color, Diffuse, FogCoord, Normal, PointSize, Position,
    Specular, Tangent, TexCoord,
};

// Vertex outputs and fragment inputs, as libgxm's masks (SceGxmVertexProgramOutput, SceGxmFragmentProgramInput).
namespace varying {
inline constexpr u32 kPosition = 1u << 0;  // (fragment inputs: the window position)
inline constexpr u32 kFog = 1u << 1;
inline constexpr u32 kColor0 = 1u << 2;
inline constexpr u32 kColor1 = 1u << 3;
inline constexpr u32 kTexCoord0 = 1u << 4;  // ... kTexCoord0 << 9
inline constexpr u32 kPointSize = 1u << 14;  // vertex outputs; for fragment inputs the sprite coordinate
inline constexpr u32 kSpriteCoord = 1u << 14;
inline constexpr u32 kClip0 = 1u << 15;  // ... kClip0 << 7 (vertex outputs only)
}  // namespace varying

struct Parameter {
    u32 offset = 0;  // of its record in the file
    nx::string name;  // empty for uniform buffers, which have none
    bool named = false;
    ParameterCategory category = ParameterCategory::Uniform;
    ParameterType type = ParameterType::F32;
    u8 components = 0;
    u32 array_size = 0;
    u32 resource = 0;  // register (attributes), offset in 32-bit words (uniforms), unit (samplers), buffer index
    u8 container = 0;  // the buffer a uniform is in: 0-13 user buffers, 14 the default uniform buffer
    Semantic semantic = Semantic::None;  // (attributes only)
    u8 semantic_index = 0;
    bool sampler_cube = false;
    bool reg_format = false;  // an attribute read in its declared format, not converted to f32
};

// How the secondary attributes are filled before a draw (by the driver, not by the program): register ranges
// ("containers") hold the default uniform buffer and a block of per-draw constants: the user uniform buffers'
// addresses, the state words of the samplers whose coordinates are computed, and the program's literals.
inline constexpr u16 kDefaultUniformContainer = 14;
inline constexpr u16 kConstantsContainer = 19;

struct Container {
    u16 index = 0;  // kDefaultUniformContainer, kConstantsContainer
    u16 base = 0;   // first secondary attribute
    u16 size = 0;   // in 32-bit registers
};

// A literal: the value of a register of the constants block (offset from its base).
struct Literal {
    u32 offset = 0;
    u32 value = 0;
};

// A uniform buffer's address goes to a register of the constants block, plus a bias in bytes: -4 for a user
// buffer; for the default uniform buffer (kDefaultUniformContainer), whose first words are in secondary attributes
// and the rest read from memory, 4 * (the first word not in a register) - 4. (Loads count their offsets from one
// element past that address.)
struct BufferSlot {
    u16 buffer = 0;  // the BUFFER[n] index, or kDefaultUniformContainer
    u16 offset = 0;
    i32 bias = 0;
};

// A word of sampler state goes to a register of the constants block.
struct SamplerSlot {
    u16 word = 0;  // sampler resource index * 4 + word
    u16 offset = 0;
};

// A fragment program input: an interpolated value (and/or a texture sampled with an interpolated coordinate
// before the program runs), placed in the primary attributes.
struct FragmentInput {
    u32 interpolated = 0;  // varying:: bit, 0 for none
    u32 sampled = 0;       // varying:: bit of the coordinate sampled with, 0 for none
    u8 format = 0;         // 0 f32, 1 fx10, 2 f16 (of the interpolated value)
    u8 components = 0;
    u16 offset = 0;        // the interpolated value's first primary attribute (32-bit register)
    // The sample (when `sampled`): the sampler's texture unit, the result's format (0 u8x4, 1 f16x4, 3 f32x4) and
    // first primary attribute (after the interpolated value), and whether the coordinate is projected (divided by
    // its last component).
    u8 sampler = 0;
    u8 sample_format = 0;
    u16 sample_offset = 0;
    bool projected = false;
    u32 raw[4] = {};
};

struct Gxp {
    ShaderType type = ShaderType::Vertex;
    u8 version_major = 0, version_minor = 0;
    u32 size = 0;  // the file's own size field
    bool discard = false, depth_replace = false, sprite_coord = false, native_color = false, frag_color = false;
    u32 default_uniform_buffer_size = 0;  // bytes
    u32 vertex_outputs = 0;  // varying:: mask (vertex programs)
    // Vertex programs that read the vertex's index (Cg's INDEX semantic) or the instance's (INSTANCE): the primary
    // attribute register the hardware puts it in, as a 32-bit integer (-1: not read). They are not parameters.
    i32 index_register = -1, instance_register = -1;
    // Where a vertex program writes its outputs (vertex_output_layout()).
    u32 vertex_texcoord_sizes = 0;  // 3 bits per texture coordinate: 1 two registers, 3 three, 7 four
    u32 vertex_output_registers = 0;  // all the outputs
    u32 fragment_inputs = 0;  // varying:: mask (fragment programs)
    nx::vector<FragmentInput> inputs;  // (fragment programs, in their order in the primary attributes)
    // The fragment program's output register format (fragment programs only).
    ParameterType output_type = ParameterType::F32;
    u8 output_components = 0;
    nx::vector<Parameter> parameters;
    nx::vector<Container> containers;
    nx::vector<Literal> literals;
    nx::vector<BufferSlot> buffer_slots;
    nx::vector<SamplerSlot> sampler_slots;
    nx::vector<u64> primary;
    nx::vector<u64> secondary;

    // As sceGxmProgramFindParameterByName / BySemantic (the index in parameters, or none).
    std::optional<usize> find(nx::string_view name) const;
    std::optional<usize> find(Semantic semantic, u32 index) const;
    const Container* container(u16 index) const;
};

// A vertex program's outputs in its output registers, in this order (checked with the shader compiler's programs):
// the position (four), colours 0 and 1 (four each), fog (two), texture coordinates 0-9 (two for one or two
// components, three, or four), the point size (one), then each clip plane used (one). Every register holds an f32,
// whatever the output's declared precision. The first register of each output (-1 where it is not written).
struct VertexOutputLayout {
    i32 position = -1, color[2] = {-1, -1}, fog = -1, texcoord[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    u8 texcoord_registers[10] = {};
    i32 point_size = -1, clip[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    u32 registers = 0;
};
VertexOutputLayout vertex_output_layout(const Gxp& g);

// Whether libgxm accepts the program: "GXP\0" and version 1.4 or 1.5 (sceGxmProgramCheck).
bool gxp_valid(std::span<const u8> data);

// Reads a GXP held in memory (everything is copied out).
Result<Gxp> parse_gxp(std::span<const u8> data);

}  // namespace psvrecore::usse
