#pragma once

// Textures made on the GPU from a render target's image: a surface a scene drew and wrote back to guest memory,
// which a later draw samples as a texture (a depth surface read as F32M, an HDR colour surface as U2F10F10F10, a
// D16 shadow map as U16). Instead of the texels being read back from guest memory, decoded on the CPU and uploaded
// again, a compute shader makes them from the image: each texel's word as the write-back puts it in guest memory
// (the same packing: pack_u2f10f10f10, depth16_of), then decoded as read_texels decodes it (its tables and the
// swizzle's choices are the decoder's own, found by decoding probes), to the same bytes. Texels the image does not
// hold (a texture larger than what the scene stored) come from a copy of guest memory, the background.
//
// The shader (target_texture_shader, 8 x 8 invocations, one a texel) reads three storage buffers of set 0:
//   0: the work buffer: the image's texels as copied from it (words), and the texels it writes at `out`;
//   1: the tables (target_texture_tables);
//   2: the background: guest memory's bytes from the texture's address (unread when it has none).

#include <optional>
#include <span>

#include "psvrecore/gxm/texture.hpp"
#include "psvrecore/vulkan/device.hpp"
#include "psvrecore/vulkan/resources.hpp"

namespace psvrecore::gxm {

// What an image's texels are and how a write-back makes guest memory's words of them.
enum class TargetSource : u32 {
    Depth32 = 0,      // a D32 float depth aspect's words, written back as they are (DF32, DF32M but for the mask bit)
    Depth16 = 1,      // the same, written back as 16-bit unorm (D16): depth16_of
    U2F10F10F10 = 2,  // R16G16B16A16_SFLOAT halves (two words a texel), packed: pack_u2f10f10f10
};

// The write-back's conversions (the renderer's, shared with the shader's checks).
// Four halves (R, G, B, A) to a U2F10F10F10_ABGR word: R, G and B rounded to unsigned 10-bit floats (to nearest,
// ties to even; negative values 0, large ones the largest finite value, infinity and NaN kept), A clamped to 0-1 and
// rounded to two bits.
u32 pack_u2f10f10f10(const u16 h[4]);
// The same for `count` pixels (four halves each, to a word each), in integer arithmetic only: the alpha's rounding
// is pack_u2f10f10f10's, by the half values where it steps (a surface's write-back, each frame).
void pack_u2f10f10f10(const u16* halves, u32* words, usize count);
// A depth value as a D16 surface's sample: rounded to nearest (halves away from zero), the low 16 bits kept.
u16 depth16_of(f32 z);

// What read_texels makes of a texel's word, for the shader to make the same.
struct TargetDecode {
    bool single = false;  // one 32-bit float a texel (an R32F image whose view's components are rgba_of), else four
                          // halves (RGBA16F)
    u8 rgba_of[4] = {};   // (single: as Texels::rgba_of)
    u32 kind = 0;         // 0: the word, masked; 1: a U16's half from the table; 2: U2F10F10F10's fields from the tables
    u32 mask = ~0u;
    u32 select = 0;       // (kinds 1-2: four fields of 4 bits, an output half's source: 0-3 a component, 4 zero, 5 one)
    bool alpha_low = false;  // (U2F10F10F10: the alpha in the low two bits, by the swizzle)
};
// The decode of a texture of this format read from an image of `source`: none for a format, gamma or palette the
// shader does not make, or whose probes the decoder decodes otherwise than expected. (Probes the decoder: called
// once for a format and swizzle, then remembered by the caller.)
std::optional<TargetDecode> target_decode(const TextureInfo& info, TargetSource source);

// The shader's push constants.
struct TargetTextureConstants {
    u32 width = 0, height = 0;      // the texels made: rows of `width` at word `out` of the work buffer
    u32 a_width = 0, a_height = 0;  // those (x < a_width, y < a_height) from the image's texels; the rest from the background
    // The image's texels at word `src` of the work buffer, rows of src_pitch texels; texel x of a row at
    // src_first + x * src_step (a scaled image's middle samples).
    u32 src = 0, src_pitch = 0, src_step = 1, src_first = 0;
    u32 out = 0;
    u32 source = 0;  // TargetSource
    u32 kind = 0, mask = ~0u, select = 0, alpha_low = 0;  // (TargetDecode)
    u32 alpha1 = 0, alpha2 = 0, alpha3 = 0;  // (U2F10F10F10 packing: the least half bits whose 2-bit alpha is 1, 2, 3)
    // The background: texel j at word bg of buffer 2 (16-bit texels: half words), j = y * bg_pitch + x in rows, or
    // in 32 x 32 tiles of bg_pitch tiles a row.
    u32 bg = 0, bg_tiled = 0, bg_pitch = 0, bg_bits = 32;
};
// The constants' packing fields (alpha1-3) for a source.
void target_packing(TargetTextureConstants& k);
// Words a texel of the image takes in the work buffer, and that the shader writes.
inline u32 target_source_words(TargetSource s) { return s == TargetSource::U2F10F10F10 ? 2 : 1; }
inline u32 target_out_words(const TargetDecode& d) { return d.single ? 1 : 2; }

nx::vector<u32> target_texture_shader();
// The decoder's tables (buffer 1): the U16 values' halves (65536), the 10-bit floats' (1024), the 2-bit alphas' (4).
const nx::vector<u32>& target_texture_tables();

// The shader's pipeline on a device, its tables in a device buffer, and a pool of its descriptor sets.
struct TargetConverter {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vulkan::DeviceBuffer tables{};
    bool ready() const { return pipeline != VK_NULL_HANDLE; }
};
Result<void> create_target_converter(vulkan::Device& device, TargetConverter& out);
void destroy_target_converter(vulkan::Device& device, TargetConverter& c);
// A set for one conversion (null: the pool has none free now): the work buffer, and the background's (or, with none,
// the work buffer again).
VkDescriptorSet target_converter_set(vulkan::Device& device, TargetConverter& c, VkBuffer work, VkDeviceSize work_bytes,
                                     VkBuffer background, VkDeviceSize background_bytes);
void free_target_converter_set(vulkan::Device& device, TargetConverter& c, VkDescriptorSet set);
void record_target_conversion(VkCommandBuffer cmd, const TargetConverter& c, VkDescriptorSet set,
                              const TargetTextureConstants& k);

// One conversion run on the device and waited for (the tests' and the start-up check's): the image's texels as the
// work buffer holds them at `src`, the background's bytes; the texels made (width * height * out words).
Result<nx::vector<u32>> run_target_conversion(vulkan::Device& device, TargetConverter& c, const TargetTextureConstants& k,
                                              std::span<const u32> image, std::span<const u8> background);

// The start-up check: textures of each source made on the device from images of chosen and random values (with a
// background), compared with read_texels of guest memory as the write-back leaves it. False: they differ (the
// device's arithmetic is not the CPU's), and textures are read back and decoded as before. PSVRECORE_TARGET_TEXTURES=0
// turns the GPU's textures off.
struct TargetCheck {
    bool ran = false, ok = false;
    nx::string what;  // (the first difference)
    f64 ms = 0;
};
TargetCheck check_target_conversion(vulkan::Device& device, TargetConverter& c);

}  // namespace psvrecore::gxm
