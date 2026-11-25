#pragma once

// Textures made on the GPU from a render target's image: a surface a scene drew and wrote back to guest memory,
// which a later draw samples as a texture (a depth surface read as F32M, an HDR colour surface as U2F10F10F10, a
// D16 shadow map as U16, an S8D24 one as X8U24). Instead of the texels being read back from guest memory, decoded on
// the CPU and uploaded again, a compute shader makes them from the image: each texel's word as the write-back puts it
// in guest memory (the same packing: pack_u2f10f10f10, depth16_of, depth24_of), then decoded as read_texels decodes it (its tables and the
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
    // A D32 float depth aspect's words and the stencil aspect's bytes (at word `stencil`, rows of stencil_pitch bytes),
    // written back as S8D24's packed words: depth24_of
    Depth24S8 = 3,
    // A surface's words as guest memory will hold them (a pending scene's stores, from its memory buffer), passed
    // through: X8U24 decoded as Depth24S8's
    Words = 4,
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
// A depth value and stencil byte as an S8D24 surface's word: the depth rounded to 24-bit unorm (to nearest, halves
// away from zero), the stencil the top byte.
u32 depth24_of(f32 z, u8 stencil);

// What read_texels makes of a texel's word, for the shader to make the same.
struct TargetDecode {
    bool single = false;  // one 32-bit float a texel (an R32F image whose view's components are rgba_of), else four
                          // halves (RGBA16F)
    u8 rgba_of[4] = {};   // (single: as Texels::rgba_of)
    u32 kind = 0;         // 0: the word, masked; 1: a U16's half from the table; 2: U2F10F10F10's fields from the tables;
                          // 3: X8U24's: the low 24 bits' half (found among the table's steps), the top byte's
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
    // (Depth24S8: the stencil's bytes from word `stencil` of the work buffer, rows of stencil_pitch bytes; texel x of a
    // row at src_first + x * src_step, as the depth's)
    u32 stencil = 0, stencil_pitch = 0;
};
// The constants' packing fields (alpha1-3) for a source.
void target_packing(TargetTextureConstants& k);
// An HDR surface's write-back on the GPU: a width x height image's texels (RGBA16F halves, rows of `width`, from
// word 0 of the work buffer) to the U2F10F10F10 words guest memory gets (pack_u2f10f10f10), one a pixel from word
// `out` (16-byte aligned, past the texels).
TargetTextureConstants target_pack_constants(u32 width, u32 height);
// Words a texel of the image takes in the work buffer, and that the shader writes.
inline u32 target_source_words(TargetSource s) { return s == TargetSource::U2F10F10F10 ? 2 : 1; }
inline u32 target_out_words(const TargetDecode& d) { return d.single ? 1 : 2; }

nx::vector<u32> target_texture_shader();
// The decoder's tables (buffer 1): the U16 values' halves (65536), the 10-bit floats' (1024), the 2-bit alphas' (4);
// X8U24's: the least 24-bit value whose half is at least h, for each h (16384, past 1.0 none), and the top byte's
// halves (256).
const nx::vector<u32>& target_texture_tables();

// An HDR surface's texture made from its image directly (target_image_shader, 8 x 8 invocations, one a texel): the
// image's halves fetched (set 0 binding 0, a sampled RGBA16F image), packed as the write-back packs them, decoded as
// target_texture_shader decodes U2F10F10F10 (binding 1: the tables), written to the texture (binding 2, an RGBA16F
// storage image). No copies through buffers; texels (x, y) of the texture from (x0 + x, y0 + y) of the image. The
// halves are fetched as floats: the same bits but a NaN's, whose sign the device may drop (a negative NaN packs to
// 0, a positive one to a NaN).
struct TargetImageConstants {
    u32 width = 0, height = 0, x0 = 0, y0 = 0;
    u32 select = 0, alpha_low = 0;           // (TargetDecode)
    u32 alpha1 = 0, alpha2 = 0, alpha3 = 0;  // (target_packing)
};
nx::vector<u32> target_image_shader();

// An S8D24 surface's words (set 0 binding 0, a storage buffer: `count` of them from word `raw`) made a depth image's
// aspects as read_depth_surface makes them: the depth words from word 0 (D32_SFLOAT: (w & 0xFFFFFF) / 16777215.0f,
// rounded as the CPU rounds it, in integers: that quotient's bits repeat the 24 bits, so it is the bits from the top
// one up, rounded up; a D24_UNORM_S8_UINT image's: the low 24 bits), the stencil bytes (their top bytes) from word
// `stencil`. Four words an invocation: from word 0 up to `count` rounded up to four, the depth written.
struct Depth24Constants {
    u32 count = 0, unorm24 = 0, stencil = 0, raw = 0;
};
// (the words: count rounded up to four, a quarter of that, then the surface's words)
Depth24Constants depth24_constants(u32 count, bool unorm24);
nx::vector<u32> depth24_shader();

// The shaders' pipelines on a device, the tables in a device buffer, and pools of their descriptor sets.
struct TargetConverter {
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vulkan::DeviceBuffer tables{};
    // (target_image_shader's: none where the start-up check found its texels differ)
    VkDescriptorSetLayout image_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout image_layout = VK_NULL_HANDLE;
    VkPipeline image_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool image_pool = VK_NULL_HANDLE;
    // (depth24_shader's)
    VkDescriptorSetLayout depth_set_layout = VK_NULL_HANDLE;
    VkPipelineLayout depth_layout = VK_NULL_HANDLE;
    VkPipeline depth_pipeline = VK_NULL_HANDLE;
    VkDescriptorPool depth_pool = VK_NULL_HANDLE;
    bool ready() const { return pipeline != VK_NULL_HANDLE; }
    bool depth_ready() const { return depth_pipeline != VK_NULL_HANDLE; }
    bool images_ready() const { return image_pipeline != VK_NULL_HANDLE; }
};
Result<void> create_target_converter(vulkan::Device& device, TargetConverter& out);
void destroy_target_converter(vulkan::Device& device, TargetConverter& c);
// target_image_shader's pipeline alone (images_ready() false after).
void destroy_target_image_converter(vulkan::Device& device, TargetConverter& c);
// A set for one conversion (null: the pool has none free now): the work buffer, and the background's (or, with none,
// the work buffer again).
VkDescriptorSet target_converter_set(vulkan::Device& device, TargetConverter& c, VkBuffer work, VkDeviceSize work_bytes,
                                     VkBuffer background, VkDeviceSize background_bytes);
void free_target_converter_set(vulkan::Device& device, TargetConverter& c, VkDescriptorSet set);
void record_target_conversion(VkCommandBuffer cmd, const TargetConverter& c, VkDescriptorSet set,
                              const TargetTextureConstants& k);
// target_image_shader's: a set (null: none free now), its views written (when recording: the image's view then),
// given back; the conversion recorded (the image in SHADER_READ_ONLY_OPTIMAL, the texture in GENERAL).
VkDescriptorSet target_image_set(vulkan::Device& device, TargetConverter& c);
void write_target_image_set(vulkan::Device& device, const TargetConverter& c, VkDescriptorSet set, VkImageView image,
                            VkImageView texture);
void free_target_image_set(vulkan::Device& device, TargetConverter& c, VkDescriptorSet set);
void record_target_image_conversion(VkCommandBuffer cmd, const TargetConverter& c, VkDescriptorSet set,
                                    const TargetImageConstants& k);
// depth24_shader's: a set for the work buffer (null: none free now), given back; the conversion recorded.
VkDescriptorSet target_depth_set(vulkan::Device& device, TargetConverter& c, VkBuffer words, VkDeviceSize bytes);
void free_target_depth_set(vulkan::Device& device, TargetConverter& c, VkDescriptorSet set);
void record_depth24_conversion(VkCommandBuffer cmd, const TargetConverter& c, VkDescriptorSet set, const Depth24Constants& k);
// (the tests': the words converted on the device and waited for: the depth words, then the stencil bytes)
Result<std::pair<nx::vector<u32>, nx::vector<u8>>> run_depth24_conversion(vulkan::Device& device, TargetConverter& c,
                                                                          std::span<const u32> words, bool unorm24);

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
// The same for target_image_shader: HDR images of halves of every kind, each swizzle, against read_texels of their
// packed words. Not ok: its pipeline is destroyed (images_ready() false), the buffer conversion used instead.
TargetCheck check_target_image_conversion(vulkan::Device& device, TargetConverter& c);

}  // namespace psvrecore::gxm
