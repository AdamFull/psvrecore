// Rendering a scene with the Vulkan backend, when it ends (synchronously: the scene's results are in guest memory
// when sceGxmEndScene returns).
//
// - The colour surface is an image of the surface's format, loaded from guest memory at the start (GXM loads
//   colour at the start of every scene) and written back at the end.
// - Each draw's secondary programs run first, one compute dispatch each, on the draw's secondary attributes (set
//   up as the driver does: gxm::fill_secondary). Guest memory the programs read (uniform buffers) is copied into
//   one buffer per scene, and its addresses in the secondary attributes are offsets into that buffer.
// - Then one render pass: per draw a pipeline (cached) from the translated programs and the draw's state, its
//   vertex and index data copied from guest memory.
//
// - Textures are decoded on the CPU (gxm/texture.hpp) into RGBA images, cached by their control words and a hash
//   of their data; units a program declares but the draw leaves unset read a white texel.
//
// - Depth and stencil: an image per scene, cleared to the surface's background values; a surface in force load
//   or force store mode is loaded from guest memory at the start or stored back at the end (linear or tiled,
//   every format but the DF32M mask bits, which are not tracked: stored as the background mask).
// - A scene without a colour surface (depth only) renders at the render target's size.
//
// - Visibility testing: an occlusion query per draw that tests it; when the scene is done, its count is added to
//   (or, SET, sets to 1) the counter in core 0's visibility buffer (the game sums the cores' buffers).
//
// Not yet: tiled and swizzled colour surfaces, multisampling.

#include <bit>
#include <cmath>
#include <cstring>

#include "psvrecore/gxm/texture.hpp"
#include "psvrecore/gxm/translate.hpp"
#include "psvrecore/log.hpp"
#include "psvrecore/vulkan/pipelines.hpp"
#include "state.hpp"
#include "core/foundation/core/hash.h"
#include "core/foundation/strings/format.h"

namespace psvrecore::gxm::hle {

namespace {

namespace vk = vulkan;

runtime::GuestMemory& memory() { return runtime::runtime().memory(); }

// ---- Formats ---------------------------------------------------------------------------------------------------

struct ColorFormat {
    VkFormat format = VK_FORMAT_UNDEFINED;
    u32 bytes = 0;
    u8 swizzle[4] = {0, 1, 2, 3};  // (FragmentShader::swizzle)
};

// The image format whose memory layout is the surface's, and the order the output register's lanes go to it.
bool color_format(u32 gxm, ColorFormat& out) {
    switch (gxm) {
        case 0x00000000u: out = {VK_FORMAT_R8G8B8A8_UNORM, 4, {0, 1, 2, 3}}; return true;      // U8U8U8U8_ABGR
        case 0x00100000u: out = {VK_FORMAT_B8G8R8A8_UNORM, 4, {0, 1, 2, 3}}; return true;      // U8U8U8U8_ARGB
        case 0x00200000u: out = {VK_FORMAT_R8G8B8A8_UNORM, 4, {3, 2, 1, 0}}; return true;      // U8U8U8U8_RGBA
        case 0x00300000u: out = {VK_FORMAT_R8G8B8A8_UNORM, 4, {3, 0, 1, 2}}; return true;      // U8U8U8U8_BGRA
        case 0x01000000u: out = {VK_FORMAT_R16G16B16A16_SFLOAT, 8, {0, 1, 2, 3}}; return true;  // F16x4 ABGR
        case 0x00800000u: out = {VK_FORMAT_R16G16_SFLOAT, 4, {0, 1, 4, 5}}; return true;       // F16F16_GR
        case 0x10800000u: out = {VK_FORMAT_R32_SFLOAT, 4, {0, 4, 4, 5}}; return true;          // F32_R
        case 0x11000000u: out = {VK_FORMAT_R32G32_SFLOAT, 8, {0, 1, 4, 5}}; return true;       // F32F32_GR
        default: return false;
    }
}

// The Vulkan input format of an attribute (vertex_input_read: how the shader takes it). Three 8- or 16-bit
// components are fetched as four (vertex formats with three such components are often not supported).
VkFormat attribute_format(const VertexInput& in) {
    const u32 n = nx::clamp<u32>(in.components, 1, 4);
    const VertexRead read = vertex_input_read(in);
    auto pick = [&](const VkFormat* four) { return four[n - 1]; };
    static const VkFormat u8[] = {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_UINT};
    static const VkFormat s8[] = {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8A8_SINT, VK_FORMAT_R8G8B8A8_SINT};
    static const VkFormat u16[] = {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16A16_UINT, VK_FORMAT_R16G16B16A16_UINT};
    static const VkFormat s16[] = {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SINT};
    static const VkFormat un8[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    static const VkFormat sn8[] = {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_SNORM};
    static const VkFormat un16[] = {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_UNORM};
    static const VkFormat sn16[] = {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_SNORM};
    static const VkFormat f16[] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
    static const VkFormat f32[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat u32f[] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT};
    if (read == VertexRead::Raw) {
        const u32 bits = vertex_input_bits(in.format);
        return pick(bits == 8 ? u8 : bits == 16 ? u16 : u32f);
    }
    switch (in.format) {
        case 0: return pick(u8);
        case 1: return pick(s8);
        case 2: return pick(u16);
        case 3: return pick(s16);
        case 4: return pick(un8);
        case 5: return pick(sn8);
        case 6: return pick(un16);
        case 7: return pick(sn16);
        case 8: return pick(f16);
        default: return pick(f32);
    }
}

// Bytes an attribute reads from a vertex (as its Vulkan format fetches them).
u32 attribute_bytes(const VertexInput& in) {
    const u32 n = nx::clamp<u32>(in.components, 1, 4);
    const u32 bits = vertex_input_bits(in.format);
    return bits == 32 ? 4 * n : (bits / 8) * (n == 3 ? 4 : n);
}

VkBlendFactor blend_factor(u32 f) {
    static const VkBlendFactor kMap[] = {
        VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,           VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE,
        VK_BLEND_FACTOR_ONE,  // (DST_ALPHA_SATURATE: Vulkan has none)
    };
    return f < std::size(kMap) ? kMap[f] : VK_BLEND_FACTOR_ONE;
}

// ---- Depth/stencil surfaces in guest memory --------------------------------------------------------------------

struct DepthLayout {
    u32 depth_bytes = 0;  // per sample at the depth address (0: no depth)
    bool stencil = false;  // a stencil byte per sample at the stencil address
    bool packed = false;   // S8D24: the stencil is the depth word's top byte
    bool mask = false;     // DF32M: the sign bit is the mask
};

bool depth_layout(u32 format, DepthLayout& out) {
    switch (format) {
        case 0x0044000u: out = {4, false, false, false}; return true;  // DF32
        case 0x00CC000u: out = {4, false, false, true}; return true;   // DF32M
        case 0x0066000u: out = {4, true, false, false}; return true;   // DF32_S8
        case 0x00EE000u: out = {4, true, false, true}; return true;    // DF32M_S8
        case 0x0022000u: out = {0, true, false, false}; return true;   // S8
        case 0x1266000u: out = {4, false, true, false}; return true;   // S8D24
        case 0x2444000u: out = {2, false, false, false}; return true;  // D16
        default: return false;
    }
}

// The index of a sample: rows of the stride (in samples), or 32 x 32 tiles in rows, each tile's samples in rows.
u32 depth_sample(const DepthStencilSurface& s, u32 x, u32 y) {
    const u32 stride = (s.background_control >> 16) * 32;
    if (!(s.control & kDsTiled)) return y * stride + x;
    return ((y / 32) * (stride / 32) + x / 32) * 1024 + (y % 32) * 32 + x % 32;
}

// Samples a surface of this size spans.
u32 depth_samples(const DepthStencilSurface& s, u32 width, u32 height) {
    const u32 stride = (s.background_control >> 16) * 32;
    if (s.control & kDsTiled) return (height + 31) / 32 * (stride / 32) * 1024;
    return (height - 1) * stride + width;
}

bool depth_surface_valid(const DepthStencilSurface& s, const DepthLayout& l, u32 width, u32 height) {
    const u32 n = depth_samples(s, width, height);
    if (l.depth_bytes && !memory().valid(s.depth_data, n * l.depth_bytes, true)) return false;
    if (l.stencil && !memory().valid(s.stencil_data, n, true)) return false;
    return true;
}

// Whether the depth image's stencil aspect holds the mask bit (bit 0): DF32M surfaces, which have no stencil.
bool mask_in_stencil(const DepthLayout& l) { return l.mask && !l.stencil; }

// The surface's samples as the depth image's aspects take them: depth (f32, or 24-bit unorm words for a
// D24_UNORM_S8_UINT image) and stencil bytes (or the mask bits).
void read_depth_surface(const DepthStencilSurface& s, const DepthLayout& l, u32 width, u32 height, bool unorm24,
                        u32* depth, u8* stencil) {
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 i = depth_sample(s, x, y), at = y * width + x;
            f32 z = s.background_depth;
            u8 st = static_cast<u8>(mask_in_stencil(l) ? (s.background_control >> 8) & 1 : s.background_control & 0xFF);
            if (l.depth_bytes == 4) {
                u32 w;
                std::memcpy(&w, memory().host(s.depth_data + i * 4), 4);
                if (l.packed) z = static_cast<f32>(w & 0xFFFFFF) / 16777215.0f, st = static_cast<u8>(w >> 24);
                else z = std::bit_cast<f32>(l.mask ? w & 0x7FFFFFFFu : w);
                if (mask_in_stencil(l)) st = static_cast<u8>(w >> 31);
            } else if (l.depth_bytes == 2) {
                u16 h;
                std::memcpy(&h, memory().host(s.depth_data + i * 2), 2);
                z = static_cast<f32>(h) / 65535.0f;
            }
            if (l.stencil) st = *memory().host(s.stencil_data + i);
            z = nx::clamp(std::isnan(z) ? 0.0f : z, 0.0f, 1.0f);
            depth[at] = unorm24 ? static_cast<u32>(std::lround(z * 16777215.0f)) : std::bit_cast<u32>(z);
            stencil[at] = st;
        }
    }
}

// And back. (DF32M_S8: the mask bit of every sample is the surface's background mask.)
void write_depth_surface(const DepthStencilSurface& s, const DepthLayout& l, u32 width, u32 height, bool unorm24,
                         const u32* depth, const u8* stencil) {
    const u32 mask = l.mask && (s.background_control & 0x100) ? 0x80000000u : 0;
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const u32 i = depth_sample(s, x, y), at = y * width + x;
            const f32 z = unorm24 ? static_cast<f32>(depth[at] & 0xFFFFFF) / 16777215.0f : std::bit_cast<f32>(depth[at]);
            if (l.depth_bytes == 4) {
                const u32 w = l.packed ? static_cast<u32>(std::lround(z * 16777215.0f)) | u32{stencil[at]} << 24
                                       : std::bit_cast<u32>(z) | (mask_in_stencil(l) ? u32{stencil[at] & 1u} << 31 : mask);
                std::memcpy(memory().host(s.depth_data + i * 4), &w, 4);
            } else if (l.depth_bytes == 2) {
                const u16 h = static_cast<u16>(std::lround(z * 65535.0f));
                std::memcpy(memory().host(s.depth_data + i * 2), &h, 2);
            }
            if (l.stencil) *memory().host(s.stencil_data + i) = stencil[at];
        }
    }
}

// ---- The renderer ----------------------------------------------------------------------------------------------

struct Target {
    u32 address = 0, format = 0, width = 0, height = 0;
    vk::Image image;
};

struct CachedTexture {
    vk::Image image;
    VkSampler sampler = VK_NULL_HANDLE;
    u64 used = 0;  // (the scene it was last used in)
};

struct Renderer {
    bool tried = false;
    nx::unique_ptr<vk::Device> device;
    nx::unique_ptr<vk::RenderPasses> passes;
    nx::unique_ptr<vk::Pipelines> pipelines;
    nx::unique_ptr<vk::Samplers> samplers;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    nx::hash_map<u64, VkShaderModule> modules;      // (by program and variant)
    nx::hash_map<u64, VkPipeline> compute;          // secondary programs (by program)
    nx::hash_map<u64, nx::string> failed_programs;  // (reported once)
    nx::vector<Target> targets;
    vk::Image white, white_cube;
    VkSampler sampler = VK_NULL_HANDLE;
    nx::hash_map<u64, CachedTexture> textures;
    nx::hash_map<u64, nx::string> failed_textures;
    u64 scenes = 0;
};

Renderer& renderer() {
    static Renderer r;
    return r;
}

bool ensure(Renderer& r) {
    if (r.tried) return static_cast<bool>(r.device);
    r.tried = true;
    auto device = vk::Device::create(state().options->device);
    if (!device) {
        nx::loge(logs::gpu, "no Vulkan device: {}; scenes are not rendered", device.error().message);
        return false;
    }
    r.device = std::move(*device);
    vk::Device& d = *r.device;
    r.passes = nx::make_unique<vk::RenderPasses>(d);
    r.pipelines = nx::make_unique<vk::Pipelines>(d, *r.passes);
    r.samplers = nx::make_unique<vk::Samplers>(d);
    VkDescriptorSetLayoutBinding bindings[kDrawBindings]{};
    for (u32 i = 0; i < kDrawBindings; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i < kDrawBindingFragmentTextures ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                     : i == kDrawBindingColor           ? VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT
                                                                        : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = i == kDrawBindingColor ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                        : VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                                                              VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = kDrawBindings;
    si.pBindings = bindings;
    vkCreateDescriptorSetLayout(d.device(), &si, nullptr, &r.set_layout);
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                   sizeof(DrawConstants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &r.set_layout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    vkCreatePipelineLayout(d.device(), &li, nullptr, &r.layout);
    vk::ImageDesc wd;
    wd.width = wd.height = 1;
    if (auto img = vk::create_image(d, wd)) {
        r.white = *img;
        const u8 texel[4] = {255, 255, 255, 255};
        (void)vk::upload_image(d, r.white, texel);
    }
    vk::ImageDesc cd;
    cd.width = cd.height = 1;
    cd.layers = 6;
    cd.cube = true;
    if (auto img = vk::create_image(d, cd)) {
        r.white_cube = *img;
        const u8 texel[4] = {255, 255, 255, 255};
        for (u32 f = 0; f < 6; ++f) (void)vk::upload_image(d, r.white_cube, texel, f);
    }
    vk::SamplerDesc sd;
    if (auto s = r.samplers->get(sd)) r.sampler = *s;
    return true;
}

VkShaderModule module_for(Renderer& r, u64 key, auto&& translate) {
    if (auto it = r.modules.find(key); it != r.modules.end()) return it->second;
    if (r.failed_programs.contains(key)) return VK_NULL_HANDLE;
    auto spirv = translate();
    if (!spirv) {
        nx::logw(logs::gxm, "program not translated: {}", spirv.error().message);
        r.failed_programs[key] = spirv.error().message;
        return VK_NULL_HANDLE;
    }
    auto m = vk::create_shader(*r.device, *spirv);
    if (!m) {
        r.failed_programs[key] = m.error().message;
        return VK_NULL_HANDLE;
    }
    r.modules[key] = *m;
    return *m;
}

u64 hash_of(std::initializer_list<u64> parts, const void* bytes = nullptr, usize size = 0) {
    nx::fnv1a64 h;
    for (u64 p : parts) h.combine(p);
    if (bytes) h.combine_bytes(static_cast<const char*>(bytes), size);
    return h.value();
}

// The compute pipeline of a secondary program (VK_NULL_HANDLE: none to run, or not translated).
VkPipeline secondary_pipeline(Renderer& r, const Program& p) {
    if (p.secondary.empty()) return VK_NULL_HANDLE;
    const u64 key = hash_of({reinterpret_cast<u64>(&p), 1});
    if (auto it = r.compute.find(key); it != r.compute.end()) return it->second;
    const VkShaderModule m = module_for(r, key, [&] { return translate_secondary(p.gxp, p.secondary); });
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (m) {
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = m;
        ci.stage.pName = "main";
        ci.layout = r.layout;
        if (vkCreateComputePipelines(r.device->device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS)
            pipeline = VK_NULL_HANDLE;
    }
    r.compute[key] = pipeline;
    return pipeline;
}

vk::Image* color_target(Renderer& r, u32 address, u32 format, VkFormat vk_format, u32 width, u32 height) {
    for (Target& t : r.targets)
        if (t.address == address && t.format == format && t.width == width && t.height == height) return &t.image;
    vk::ImageDesc d;
    d.format = vk_format;
    d.width = width;
    d.height = height;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    auto img = vk::create_image(*r.device, d);
    if (!img) return nullptr;
    r.targets.push_back(Target{address, format, width, height, *img});
    return &r.targets.back().image;
}

VkSamplerAddressMode address_mode(u32 m) {
    switch (m) {
        case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 0:
        case 4: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;  // (clamp; mirror clamp needs an extension)
    }
}

// Uploads decoded texels: each face's levels.
Result<void> upload_texels(vk::Device& d, vk::Image& image, const Texels& t) {
    auto staging = vk::create_host_buffer(d, t.data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!staging) return nx::err(staging.error());
    std::memcpy(staging->mapped, t.data.data(), t.data.size());
    vk::flush(d, *staging);
    nx::vector<VkBufferImageCopy> regions;
    VkDeviceSize at = 0;
    const u32 bytes = t.bytes;
    for (u32 face = 0; face < t.faces; ++face) {
        for (u32 l = 0; l < t.levels; ++l) {
            const u32 w = nx::max(1u, t.width >> l), h = nx::max(1u, t.height >> l);
            VkBufferImageCopy c{};
            c.bufferOffset = at;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, face, 1};
            c.imageExtent = {w, h, 1};
            regions.push_back(c);
            at += VkDeviceSize{w} * h * bytes;
        }
    }
    vk::OneShot shot(d);
    auto cmd = shot.begin();
    Result<void> result;
    if (!cmd) {
        result = nx::err(cmd.error());
    } else {
        vk::transition(*cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(*cmd, staging->buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<u32>(regions.size()), regions.data());
        vk::transition(*cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        result = shot.submit_and_wait();
    }
    vk::destroy_buffer(d, *staging);
    return result;
}

// A texture as the draw's control words describe it (cached), or none if it cannot be read.
CachedTexture* texture_for(Renderer& r, const TextureWords& words) {
    const TextureInfo info = decode_texture(words.words);
    if (!info.address) return nullptr;
    const u32 size = texture_size(info);
    if (!size || !memory().valid(info.address, size)) return nullptr;
    nx::fnv1a64 h;
    h.combine_bytes(reinterpret_cast<const char*>(words.words), sizeof words.words);
    h.combine_bytes(reinterpret_cast<const char*>(memory().host(info.address)), size);
    if (info.palette && memory().valid(info.palette, 1024))
        h.combine_bytes(reinterpret_cast<const char*>(memory().host(info.palette)), 1024);
    const u64 key = h.value();
    if (auto it = r.textures.find(key); it != r.textures.end()) {
        it->second.used = r.scenes;
        return &it->second;
    }
    if (r.failed_textures.contains(key)) return nullptr;
    auto texels = read_texels(info, [](u32 address, void* out, u32 bytes) {
        if (!memory().valid(address, bytes)) return false;
        std::memcpy(out, memory().host(address), bytes);
        return true;
    });
    if (!texels) {
        nx::logw(logs::gxm, "{} (format 0x{:08x}, type {})", texels.error().message, info.base_format, info.type);
        r.failed_textures[key] = texels.error().message;
        return nullptr;
    }
    vk::ImageDesc d;
    d.format = texels->bytes == 16 ? VK_FORMAT_R32G32B32A32_SFLOAT
               : texels->bytes == 8 ? VK_FORMAT_R16G16B16A16_SFLOAT
                                    : VK_FORMAT_R8G8B8A8_UNORM;
    d.width = texels->width;
    d.height = texels->height;
    d.mips = texels->levels;
    d.layers = texels->faces;
    d.cube = texels->faces == 6;
    auto image = vk::create_image(*r.device, d);
    if (!image) return nullptr;
    if (!upload_texels(*r.device, *image, *texels)) {
        vk::destroy_image(*r.device, *image);
        return nullptr;
    }
    vk::SamplerDesc sd;
    // (32-bit float images are not filterable everywhere: nearest there)
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(r.device->physical(), d.format, &fp);
    const bool filterable = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    sd.mag = info.mag_linear && filterable ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.min = info.min_linear && filterable ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sd.mip = info.mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sd.u = address_mode(info.u_address);
    sd.v = address_mode(info.v_address);
    sd.w = sd.u;
    sd.lod_bias = (static_cast<f32>(info.lod_bias) - 31.0f) / 8.0f;  // (assumed: eighths of a level, 31 none)
    sd.max_lod = static_cast<f32>(texels->levels - 1);
    CachedTexture c;
    c.image = *image;
    if (auto smp = r.samplers->get(sd)) c.sampler = *smp;
    c.used = r.scenes;
    auto [it, added] = r.textures.emplace(key, c);
    return &it->second;
}

// Whether a fragment program's secondary program reads per-tile special registers (pixelX, pixelY, tileXY): it runs
// in the fragment shader then, in each instance (translate_fragment).
bool tile_secondary(const Program& p) {
    for (const usse::Decoded& d : p.secondary) {
        if (!d.executed) continue;
        for (u32 k = 0; k < d.in.src_count; ++k) {
            const usse::Operand& o = d.in.src[k];
            if (o.bank == usse::Bank::Special && (o.number == 87 || o.number == 88 || o.number == 107)) return true;
        }
    }
    return false;
}

// Whether a program stores to memory (its buffers are copied back after the scene).
bool stores(const Program& p) {
    for (const usse::Decoded& d : p.primary)
        if (d.executed && d.in.op == usse::Op::Store) return true;
    for (const usse::Decoded& d : p.secondary)
        if (d.executed && d.in.op == usse::Op::Store) return true;
    return false;
}

// The words of a uniform buffer a program reads (from its parameters in that buffer).
u32 buffer_words(const usse::Gxp& g, u32 index) {
    u32 words = 0;
    for (const usse::Parameter& p : g.parameters) {
        if (p.container != index || p.category != usse::ParameterCategory::Uniform) continue;
        const u32 per = (p.components + 1u) & ~1u;  // (vectors take 64-bit aligned room: an upper bound)
        words = nx::max(words, p.resource + nx::max<u32>(p.array_size, 1) * nx::max<u32>(per, 1) + 4);
    }
    return words;
}

// The bytes of a uniform buffer: the size the program declares for it (its UniformBuffer parameter), else an upper
// bound from the uniforms it reads there.
u32 buffer_bytes(const usse::Gxp& g, u32 index) {
    for (const usse::Parameter& p : g.parameters)
        if (p.category == usse::ParameterCategory::UniformBuffer && p.resource == index && p.array_size) return p.array_size;
    return buffer_words(g, index) * 4;
}

void host_copy(nx::vector<u8>& out, u32 address, u32 bytes) {
    const usize at = out.size();
    out.resize(at + ((bytes + 15) & ~15u), 0);
    if (address && bytes && memory().valid(address, bytes)) std::memcpy(out.data() + at, memory().host(address), bytes);
}

struct DrawPlan {
    const Draw* draw = nullptr;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipeline vertex_secondary = VK_NULL_HANDLE, fragment_secondary = VK_NULL_HANDLE;
    nx::vector<std::pair<u32, VkDeviceSize>> streams;  // (binding, offset in the vertex buffer)
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    DrawConstants constants;
    VkRect2D scissor{};
};

}  // namespace

void render_scene(Context& context, Scene& scene) {
    (void)context;
    Renderer& r = renderer();
    if ((!scene.has_color && !scene.has_depth) || !ensure(r)) return;
    ++r.scenes;
    vk::Device& d = *r.device;
    const ColorSurface& cs = scene.color;
    const DepthStencilSurface& ds = scene.depth;
    ColorFormat cf;
    // (without a colour surface, a depth-only scene: the render target's size)
    u32 width = scene.target->width, height = scene.target->height, stride = 0;
    vk::Image* color = nullptr;
    if (scene.has_color) {
        if (!color_format(cs.format, cf)) {
            nx::logw(logs::gxm, "colour surface format 0x{:08x} not implemented: scene not rendered", cs.format);
            return;
        }
        if (cs.modes & 3) {
            nx::logw(logs::gxm, "tiled and swizzled colour surfaces not implemented: scene not rendered");
            return;
        }
        width = cs.size & 0xFFFF, height = cs.size >> 16, stride = cs.stride;
        if (!memory().valid(cs.data, stride * cf.bytes * (height - 1) + width * cf.bytes, true)) return;
        color = color_target(r, cs.data, cs.format, cf.format, width, height);
        if (!color) return;

        // The colour as it is in guest memory (GXM loads it at the start of every scene).
        nx::vector<u8> texels(usize{width} * height * cf.bytes);
        for (u32 y = 0; y < height; ++y)
            std::memcpy(texels.data() + usize{y} * width * cf.bytes, memory().host(cs.data + y * stride * cf.bytes), usize{width} * cf.bytes);
        if (auto done = vk::upload_image(d, *color, texels, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL); !done) {
            nx::logw(logs::gpu, "{}", done.error().message);
            return;
        }
    }
    const u32 target_w = nx::min(width, scene.target->width), target_h = nx::min(height, scene.target->height);

    // Depth and stencil: an image of the scene, cleared to the surface's background values, or loaded from the
    // surface (force load) and stored back to it (force store).
    vk::Image depth;
    DepthLayout dl;
    bool has_depth = scene.has_depth && d.caps().depth_stencil_format != VK_FORMAT_UNDEFINED;
    if (has_depth && !depth_layout(ds.control & 0x03FFF000u, dl)) {
        nx::logw(logs::gxm, "depth/stencil format 0x{:08x} not implemented: no depth test", ds.control & 0x03FFF000u);
        has_depth = false;
    }
    const bool unorm24 = d.caps().depth_stencil_format == VK_FORMAT_D24_UNORM_S8_UINT;
    // The mask bit (DF32M): in the stencil aspect, tested by every draw and set by mask update draws. (DF32M_S8
    // needs the stencil aspect for the stencil: its mask is not implemented, every sample passes.)
    const bool mask_bits = has_depth && mask_in_stencil(dl);
    if (has_depth && dl.mask && dl.stencil) nx::logw(logs::gxm, "the mask bit of DF32M_S8 surfaces is not implemented");
    bool load_depth = has_depth && (ds.control & kDsForceLoad);
    bool store_depth = has_depth && (ds.control & kDsForceStore);
    if ((load_depth || store_depth) && !depth_surface_valid(ds, dl, width, height)) {
        nx::logw(logs::gxm, "depth/stencil surface outside guest memory: not loaded or stored");
        load_depth = store_depth = false;
    }
    if (has_depth) {
        vk::ImageDesc dd;
        dd.format = d.caps().depth_stencil_format;
        dd.width = width;
        dd.height = height;
        dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (auto img = vk::create_image(d, dd)) depth = *img;
    }
    if (!color && !depth.image) return;

    // ---- What the draws need: secondary attributes, memory, vertices and indices -----------------------------
    const u32 draws = static_cast<u32>(scene.draws.size());
    // (a pass with a draw that reads the colour reads it as an input attachment: every pipeline of the pass is
    // made for that render pass)
    bool reads_color = false;
    for (const Draw& dr : scene.draws)
        reads_color |= color && dr.fragment->program->gxp.native_color && dr.fragment->program->gxp.frag_color;
    nx::vector<u32> sa(usize{nx::max(draws, 1u)} * kDrawSecondaryWords, 0);
    constexpr u32 kMemoryBase = 0x1000;  // (the guest addresses the programs see for the memory buffer's words)
    nx::vector<u8> mem;  // (the memory buffer's words, after its two-word header)
    // Uniform buffers: one copy each per scene, shared by the draws that use it (so that what one draw stores, a
    // later one reads, and the copies of the buffers programs store to go back to guest memory after the scene).
    struct BufferCopy {
        u32 address = 0, bytes = 0, offset = 0;  // (offset: in mem)
        bool placed = false, written = false;
    };
    nx::hash_map<u32, BufferCopy> buffer_copies;
    for (const Draw& dr : scene.draws) {
        for (u32 stage = 0; stage < 2; ++stage) {
            const Program& p = stage ? *dr.fragment->program : *dr.vertex->program;
            const u32* buffers = stage ? dr.fragment_buffers : dr.vertex_buffers;
            const bool written = stores(p);
            for (u32 b = 0; b < 14; ++b) {
                if (!buffers[b]) continue;
                BufferCopy& c = buffer_copies[buffers[b]];
                c.address = buffers[b];
                c.bytes = nx::max(c.bytes, buffer_bytes(p.gxp, b));
                c.written |= written;
            }
        }
    }
    auto place = [&](u32 address) {
        BufferCopy& c = buffer_copies[address];
        if (!c.placed) {
            c.placed = true;
            c.offset = static_cast<u32>(mem.size());
            host_copy(mem, address, c.bytes);
        }
        return kMemoryBase + c.offset;
    };
    nx::vector<u8> vertices, indices;
    nx::vector<DrawPlan> plans;
    plans.reserve(draws);
    for (u32 i = 0; i < draws; ++i) {
        const Draw& dr = scene.draws[i];
        DrawPlan plan;
        plan.draw = &dr;
        const Program& vp = *dr.vertex->program;
        const Program& fp = *dr.fragment->program;
        // Secondary attributes: the default uniform buffers and the uniform buffers, copied into memory.
        for (u32 stage = 0; stage < 2; ++stage) {
            const Program& p = stage ? fp : vp;
            const nx::vector<u32>& uniforms = stage ? dr.fragment_uniforms : dr.vertex_uniforms;
            const u32* buffers = stage ? dr.fragment_buffers : dr.vertex_buffers;
            u32 addresses[15] = {};
            addresses[usse::kDefaultUniformContainer] = kMemoryBase + static_cast<u32>(mem.size());
            const usize at = mem.size();
            mem.resize(at + ((uniforms.size() * 4 + 15) & ~usize{15}), 0);
            if (!uniforms.empty()) std::memcpy(mem.data() + at, uniforms.data(), uniforms.size() * 4);
            for (u32 b = 0; b < 14; ++b)
                if (buffers[b]) addresses[b] = place(buffers[b]);
            fill_secondary(p.gxp, uniforms, addresses,
                           std::span<u32, 256>(sa.data() + usize{i} * kDrawSecondaryWords + 256 * stage, 256));
        }
        if ((stores(vp) || stores(fp)) && !d.caps().shader_stores) {
            nx::logw(logs::gpu, "the device has no vertex or fragment stage stores: a draw whose program stores is skipped");
            continue;
        }
        const bool inline_secondary = tile_secondary(fp);
        plan.vertex_secondary = secondary_pipeline(r, vp);
        plan.fragment_secondary = inline_secondary ? VK_NULL_HANDLE : secondary_pipeline(r, fp);

        // The shaders.
        VertexShader vs;
        for (usize a = 0; a < dr.vertex->attributes.size(); ++a) {
            const VertexAttribute& attr = dr.vertex->attributes[a];
            VertexInput in;
            in.location = static_cast<u32>(a);
            in.reg = attr.reg;
            in.format = attr.format;
            in.components = attr.components;
            for (const usse::Parameter& p : vp.gxp.parameters)
                if (p.category == usse::ParameterCategory::Attribute && p.resource == attr.reg) in.raw = p.reg_format;
            vs.inputs.push_back(in);
        }
        const u64 vkey = hash_of({reinterpret_cast<u64>(&vp), 2}, vs.inputs.data(), vs.inputs.size() * sizeof(VertexInput));
        const VkShaderModule vmod = module_for(r, vkey, [&] { return translate_vertex(vp.gxp, vp.primary, vs); });
        FragmentShader fs;
        fs.output_format = dr.fragment->output_format;
        std::memcpy(fs.swizzle, cf.swizzle, 4);
        fs.color_output = color != nullptr;
        const u64 fkey = hash_of({reinterpret_cast<u64>(&fp), 3, fs.output_format, fs.color_output ? 1u : 0u}, fs.swizzle, 4);
        const VkShaderModule fmod = module_for(r, fkey, [&] {
            const std::span<const usse::Decoded> secondary =
                inline_secondary ? std::span<const usse::Decoded>(fp.secondary) : std::span<const usse::Decoded>{};
            return translate_fragment(fp.gxp, fp.primary, fs, secondary);
        });
        if (!vmod || !fmod) continue;

        // Indices, and the vertices they reach (each stream from its first vertex to the last one used).
        const u32 index_size = dr.index_format ? 4 : 2;
        if (!memory().valid(dr.index_address, dr.index_count * index_size)) continue;
        u32 max_index = 0;
        for (u32 k = 0; k < dr.index_count; ++k) {
            const u8* p = memory().host(dr.index_address + k * index_size);
            u32 v;
            if (index_size == 4) std::memcpy(&v, p, 4);
            else {
                u16 h;
                std::memcpy(&h, p, 2);
                v = h;
            }
            max_index = nx::max(max_index, v);
        }
        plan.index_offset = indices.size();
        plan.index_type = index_size == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        host_copy(indices, dr.index_address, dr.index_count * index_size);

        auto desc = vk::GraphicsPipelineDesc::defaults();
        desc.vertex = vmod;
        desc.fragment = fmod;
        desc.layout = r.layout;
        bool ok = true;
        for (usize a = 0; a < vs.inputs.size() && ok; ++a) {
            const VertexAttribute& attr = dr.vertex->attributes[a];
            if (attr.stream >= vk::GraphicsPipelineDesc::kMaxBindings || a >= vk::GraphicsPipelineDesc::kMaxAttributes) {
                ok = false;
                break;
            }
            desc.attributes[desc.attribute_count++] = {static_cast<u32>(a), attr.stream, attribute_format(vs.inputs[a]), attr.offset};
            desc.binding_count = nx::max<u32>(desc.binding_count, attr.stream + 1u);
        }
        if (!ok) continue;
        for (u32 s = 0; s < desc.binding_count; ++s) {
            const VertexStream& st = s < dr.vertex->streams.size() ? dr.vertex->streams[s] : VertexStream{};
            const bool instanced = st.index_source >= 2;
            desc.bindings[s] = {st.stride, instanced ? 1u : 0u};
            u32 reach = 0;  // (bytes the attributes of this stream read from a vertex)
            for (usize a = 0; a < vs.inputs.size(); ++a)
                if (dr.vertex->attributes[a].stream == s)
                    reach = nx::max<u32>(reach, dr.vertex->attributes[a].offset + attribute_bytes(vs.inputs[a]));
            const u32 count = instanced ? dr.instances : max_index + 1;
            const u32 bytes = st.stride ? (count - 1) * st.stride + reach : reach;
            plan.streams.push_back({s, vertices.size()});
            host_copy(vertices, dr.streams[s], bytes);
        }
        switch (dr.primitive) {
            case 0x04000000u: desc.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
            case 0x08000000u: desc.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
            case 0x0C000000u: desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
            case 0x10000000u: desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
            default: desc.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        }
        const DrawState& st = dr.state;
        // (GXM's CW/CCW are the windings culled as they appear on the render target, which Vulkan's framebuffer
        // coordinates share: assumed until a sample with culling is checked)
        desc.front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        desc.cull = st.cull == 1 ? VK_CULL_MODE_BACK_BIT : st.cull == 2 ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
        const bool mask_update = dr.fragment->mask_update;
        if (has_depth && depth.image && mask_update) {
            // (no depth or stencil test or write; the stencil function's bit 0 is the mask bit to write)
            if (mask_bits) {
                desc.stencil_test = 1;
                desc.stencil_front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS};
                desc.stencil_back = desc.stencil_front;
            }
        } else if (has_depth && depth.image) {
            desc.depth_test = 1;
            desc.depth_compare = static_cast<VkCompareOp>((st.depth_func[0] >> 22) & 7);
            desc.depth_write = st.depth_write[0] ? 1 : 0;
            auto stencil_state = [](const StencilState& s) {
                VkStencilOpState o{};
                o.compareOp = static_cast<VkCompareOp>((s.func >> 25) & 7);
                o.failOp = static_cast<VkStencilOp>(s.fail & 7);
                o.depthFailOp = static_cast<VkStencilOp>(s.depth_fail & 7);
                o.passOp = static_cast<VkStencilOp>(s.pass & 7);
                return o;
            };
            desc.stencil_front = stencil_state(st.stencil[0]);
            desc.stencil_back = stencil_state(st.stencil[st.two_sided ? 1 : 0]);
            auto active = [](const StencilState& s) { return ((s.func >> 25) & 7) != 7 || s.fail || s.depth_fail || s.pass; };
            desc.stencil_test = active(st.stencil[0]) || (st.two_sided && active(st.stencil[1])) ? 1 : 0;
            if (mask_bits) {
                // (the mask test: samples whose mask bit is 0 are not drawn)
                desc.stencil_test = 1;
                desc.stencil_front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_EQUAL};
                desc.stencil_back = desc.stencil_front;
            }
        }
        if (dr.fragment->blend_enabled && dr.fragment->blend.color_func()) {
            const BlendInfo& b = dr.fragment->blend;
            desc.blend.enable = 1;
            desc.blend.color_op = static_cast<VkBlendOp>(b.color_func() - 1);
            desc.blend.alpha_op = static_cast<VkBlendOp>((b.alpha_func() ? b.alpha_func() : b.color_func()) - 1);
            desc.blend.src_color = blend_factor(b.color_src());
            desc.blend.dst_color = blend_factor(b.color_dst());
            desc.blend.src_alpha = blend_factor(b.alpha_src());
            desc.blend.dst_alpha = blend_factor(b.alpha_dst());
        }
        if (dr.fragment->blend_enabled) {
            const u8 m = dr.fragment->blend.color_mask;
            desc.blend.write_mask = ((m & 2) ? VK_COLOR_COMPONENT_R_BIT : 0) | ((m & 4) ? VK_COLOR_COMPONENT_G_BIT : 0) |
                                    ((m & 8) ? VK_COLOR_COMPONENT_B_BIT : 0) | ((m & 1) ? VK_COLOR_COMPONENT_A_BIT : 0);
        }
        // (a mask update program, or a disabled fragment program, writes no colour)
        if (mask_update || !st.fragment_enabled[0]) desc.blend.write_mask = 0;
        vk::Pass pass_formats;
        pass_formats.color.image = color;
        pass_formats.color_input = reads_color;
        if (has_depth && depth.image) pass_formats.depth.image = &depth;
        desc.formats = pass_formats.formats();
        auto pipeline = r.pipelines->graphics(desc);
        if (!pipeline) {
            nx::logw(logs::gpu, "{}", pipeline.error().message);
            continue;
        }
        plan.pipeline = *pipeline;

        DrawConstants& c = plan.constants;
        c.draw = i;
        c.flags = (st.viewport_enabled ? kDrawViewport : 0) | (desc.topology == VK_PRIMITIVE_TOPOLOGY_POINT_LIST ? kDrawPoints : 0);
        c.target[0] = static_cast<f32>(target_w);
        c.target[1] = static_cast<f32>(target_h);
        std::memcpy(c.viewport, st.viewport, sizeof c.viewport);
        c.w_clamp = st.w_clamp_enabled ? st.w_clamp : 0.0f;
        // Region clip (tile-aligned, as the hardware applies it) and the surface's clip, as a scissor.
        i32 x0 = 0, y0 = 0, x1 = static_cast<i32>(width) - 1, y1 = static_cast<i32>(height) - 1;
        if (color) {
            x0 = static_cast<i32>(cs.clip_min & 0xFFFF), y0 = static_cast<i32>(cs.clip_min >> 16);
            x1 = static_cast<i32>(cs.clip_max & 0xFFFF), y1 = static_cast<i32>(cs.clip_max >> 16);
        }
        if (st.region_clip_mode == 0x80000000u) {
            x0 = nx::max<i32>(x0, static_cast<i32>(st.region_min[0] & ~31u));
            y0 = nx::max<i32>(y0, static_cast<i32>(st.region_min[1] & ~31u));
            x1 = nx::min<i32>(x1, static_cast<i32>(st.region_max[0] | 31u));
            y1 = nx::min<i32>(y1, static_cast<i32>(st.region_max[1] | 31u));
        }
        x1 = nx::min<i32>(x1, static_cast<i32>(target_w) - 1);
        y1 = nx::min<i32>(y1, static_cast<i32>(target_h) - 1);
        if (x1 < x0 || y1 < y0) continue;
        plan.scissor = {{x0, y0}, {static_cast<u32>(x1 - x0 + 1), static_cast<u32>(y1 - y0 + 1)}};
        plans.push_back(std::move(plan));
    }

    // ---- Buffers ---------------------------------------------------------------------------------------------
    nx::vector<u32> mem_words(2 + mem.size() / 4 + 2, 0);
    mem_words[0] = kMemoryBase;
    mem_words[1] = static_cast<u32>(mem.size() / 4);
    if (!mem.empty()) std::memcpy(mem_words.data() + 2, mem.data(), mem.size());
    nx::vector<vk::Buffer> buffers;
    auto make = [&](const void* data, usize bytes, VkBufferUsageFlags usage) -> vk::Buffer* {
        auto b = vk::create_host_buffer(d, nx::max<usize>(bytes, 16), usage);
        if (!b) return nullptr;
        if (data && bytes) std::memcpy(b->mapped, data, bytes);
        vk::flush(d, *b);
        buffers.push_back(*b);
        return &buffers.back();
    };
    buffers.reserve(6);
    vk::Buffer* sa_buf = make(sa.data(), sa.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    vk::Buffer* mem_buf = make(mem_words.data(), mem_words.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    vk::Buffer* vb = make(vertices.data(), vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    vk::Buffer* ib = make(indices.data(), indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    const VkDeviceSize readback_bytes = color ? VkDeviceSize{width} * height * cf.bytes : 0;
    vk::Buffer* readback = make(nullptr, readback_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    // (depth and stencil, to load and store: the depth words, then the stencil bytes)
    const VkDeviceSize samples = VkDeviceSize{width} * height;
    vk::Buffer* depth_buf = nullptr;
    if (load_depth || store_depth) {
        nx::vector<u8> init;
        if (load_depth) {
            init.resize(samples * 5);
            read_depth_surface(ds, dl, width, height, unorm24, reinterpret_cast<u32*>(init.data()), init.data() + samples * 4);
        }
        depth_buf = make(load_depth ? init.data() : nullptr, samples * 5,
                         VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    auto cleanup = [&] {
        for (vk::Buffer& b : buffers) vk::destroy_buffer(d, b);
        if (depth.image) {
            r.passes->forget(depth.view);
            vk::destroy_image(d, depth);
        }
    };
    if (!sa_buf || !mem_buf || !vb || !ib || !readback || ((load_depth || store_depth) && !depth_buf)) {
        cleanup();
        return;
    }

    // ---- Descriptor sets (one per draw: the same buffers, the textures to come) ------------------------------
    VkDescriptorPool pool = VK_NULL_HANDLE;
    const u32 sets = nx::max<u32>(static_cast<u32>(plans.size()), 1);
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * sets},
                                          {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (kDrawBindings - 3) * sets},
                                          {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, sets}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = sets;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    vkCreateDescriptorPool(d.device(), &pi, nullptr, &pool);
    nx::vector<VkDescriptorSet> descriptor_sets(plans.size(), VK_NULL_HANDLE);
    for (usize i = 0; i < plans.size(); ++i) {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &r.set_layout;
        vkAllocateDescriptorSets(d.device(), &ai, &descriptor_sets[i]);
        VkDescriptorBufferInfo bi[2] = {{sa_buf->buffer, 0, VK_WHOLE_SIZE}, {mem_buf->buffer, 0, VK_WHOLE_SIZE}};
        // The units each program declares: the draw's texture there, or a white texel of the declared kind.
        VkDescriptorImageInfo ii[kDrawBindings];
        for (u32 b = kDrawBindingFragmentTextures; b < kDrawBindings; ++b)
            ii[b] = {r.sampler, r.white.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (color) ii[kDrawBindingColor] = {VK_NULL_HANDLE, color->view, VK_IMAGE_LAYOUT_GENERAL};
        for (u32 stage = 0; stage < 2; ++stage) {
            const Draw& dr = *plans[i].draw;
            const Program& p = stage ? *dr.fragment->program : *dr.vertex->program;
            for (const usse::Parameter& q : p.gxp.parameters) {
                if (q.category != usse::ParameterCategory::Sampler) continue;
                const u32 unit = q.resource;
                if (unit >= (stage ? 16u : 4u)) continue;
                const u32 binding = (stage ? kDrawBindingFragmentTextures : kDrawBindingVertexTextures) + unit;
                ii[binding].imageView = q.sampler_cube ? r.white_cube.view : r.white.view;
                const TextureWords& words = stage ? dr.fragment_textures[unit] : dr.vertex_textures[unit];
                if (CachedTexture* t = texture_for(r, words); t && t->image.desc.cube == q.sampler_cube) {
                    ii[binding].imageView = t->image.view;
                    ii[binding].sampler = t->sampler ? t->sampler : r.sampler;
                }
            }
        }
        VkWriteDescriptorSet w[kDrawBindings]{};
        const u32 writes = reads_color ? kDrawBindings : kDrawBindingColor;  // (no input attachment outside such passes)
        for (u32 b = 0; b < writes; ++b) {
            w[b] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w[b].dstSet = descriptor_sets[i];
            w[b].dstBinding = b;
            w[b].descriptorCount = 1;
            if (b < kDrawBindingFragmentTextures) {
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[b].pBufferInfo = &bi[b];
            } else if (b == kDrawBindingColor) {
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
                w[b].pImageInfo = &ii[b];
            } else {
                w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[b].pImageInfo = &ii[b];
            }
        }
        vkUpdateDescriptorSets(d.device(), writes, w, 0, nullptr);
    }

    // ---- Visibility queries ----------------------------------------------------------------------------------
    VkQueryPool queries = VK_NULL_HANDLE;
    nx::vector<u32> query_of(plans.size(), ~0u);
    u32 query_count = 0;
    if (scene.visibility_buffer)
        for (usize i = 0; i < plans.size(); ++i)
            if (plans[i].draw->state.visibility[0]) query_of[i] = query_count++;
    if (query_count) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = query_count;
        if (vkCreateQueryPool(d.device(), &qi, nullptr, &queries) != VK_SUCCESS) {
            queries = VK_NULL_HANDLE;
            query_count = 0;
        }
    }

    // ---- Commands --------------------------------------------------------------------------------------------
    vk::OneShot shot(d);
    auto cmd = shot.begin();
    if (!cmd) {
        if (queries) vkDestroyQueryPool(d.device(), queries, nullptr);
        vkDestroyDescriptorPool(d.device(), pool, nullptr);
        cleanup();
        return;
    }
    if (queries) vkCmdResetQueryPool(*cmd, queries, 0, query_count);
    const VkShaderStageFlags push_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT;
    bool computed = false;
    for (usize i = 0; i < plans.size(); ++i) {
        for (u32 stage = 0; stage < 2; ++stage) {
            const VkPipeline p = stage ? plans[i].fragment_secondary : plans[i].vertex_secondary;
            if (!p) continue;
            DrawConstants c = plans[i].constants;
            c.stage = stage;
            vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            vkCmdBindDescriptorSets(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, r.layout, 0, 1, &descriptor_sets[i], 0, nullptr);
            vkCmdPushConstants(*cmd, r.layout, push_stages, 0, sizeof c, &c);
            vkCmdDispatch(*cmd, 1, 1, 1);
            computed = true;
        }
    }
    if (computed) {
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(*cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &b, 0,
                             nullptr, 0, nullptr);
    }
    const VkBufferImageCopy depth_regions[2] = {
        {0, 0, 0, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {width, height, 1}},
        {samples * 4, 0, 0, {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, {0, 0, 0}, {width, height, 1}}};
    if (load_depth && depth.image) {
        vk::transition(*cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdCopyBufferToImage(*cmd, depth_buf->buffer, depth.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, depth_regions);
    }
    vk::Pass pass;
    if (color) pass.color = {color, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {}};
    pass.color_input = reads_color;
    if (has_depth && depth.image) {
        pass.depth.image = &depth;
        pass.depth.clear_depth = nx::clamp(ds.background_depth, 0.0f, 1.0f);
        pass.depth.clear_stencil = mask_bits ? (ds.background_control >> 8) & 1 : ds.background_control & 0xFF;
        if (load_depth) pass.depth.load = pass.depth.stencil_load = VK_ATTACHMENT_LOAD_OP_LOAD;
        if (store_depth) pass.depth.store = pass.depth.stencil_store = VK_ATTACHMENT_STORE_OP_STORE;
    }
    pass.area = {{0, 0}, {target_w, target_h}};
    if (auto begun = r.passes->begin(*cmd, pass); begun) {
        const VkViewport viewport{0, 0, static_cast<f32>(target_w), static_cast<f32>(target_h), 0, 1};
        vkCmdSetViewport(*cmd, 0, 1, &viewport);
        const f32 blend[4] = {0, 0, 0, 0};
        for (usize i = 0; i < plans.size(); ++i) {
            const DrawPlan& p = plans[i];
            const DrawState& st = p.draw->state;
            const usse::Gxp& fg = p.draw->fragment->program->gxp;
            if (fg.native_color && fg.frag_color && i > 0 && !d.caps().raster_order_attachment) {
                // (what the draws before wrote, for this one to read: the render pass's self-dependency)
                VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
                b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
                vkCmdPipelineBarrier(*cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                     VK_DEPENDENCY_BY_REGION_BIT, 1, &b, 0, nullptr, 0, nullptr);
            }
            vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
            vkCmdSetScissor(*cmd, 0, 1, &p.scissor);
            const StencilState& front = st.stencil[0];
            const StencilState& back = st.stencil[st.two_sided ? 1 : 0];
            if (mask_bits) {
                const bool update = p.draw->fragment->mask_update;
                vkCmdSetStencilReference(*cmd, VK_STENCIL_FACE_FRONT_BIT, update ? (front.func >> 25) & 1 : 1);
                vkCmdSetStencilReference(*cmd, VK_STENCIL_FACE_BACK_BIT, update ? (back.func >> 25) & 1 : 1);
                vkCmdSetStencilCompareMask(*cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 1);
                vkCmdSetStencilWriteMask(*cmd, VK_STENCIL_FACE_FRONT_AND_BACK, update ? 1 : 0);
            } else {
                vkCmdSetStencilReference(*cmd, VK_STENCIL_FACE_FRONT_BIT, front.ref);
                vkCmdSetStencilReference(*cmd, VK_STENCIL_FACE_BACK_BIT, back.ref);
                vkCmdSetStencilCompareMask(*cmd, VK_STENCIL_FACE_FRONT_BIT, front.compare_mask);
                vkCmdSetStencilCompareMask(*cmd, VK_STENCIL_FACE_BACK_BIT, back.compare_mask);
                vkCmdSetStencilWriteMask(*cmd, VK_STENCIL_FACE_FRONT_BIT, front.write_mask);
                vkCmdSetStencilWriteMask(*cmd, VK_STENCIL_FACE_BACK_BIT, back.write_mask);
            }
            vkCmdSetDepthBias(*cmd, static_cast<f32>(st.depth_bias_units[0]), 0.0f, static_cast<f32>(st.depth_bias_factor[0]));
            vkCmdSetBlendConstants(*cmd, blend);
            vkCmdBindDescriptorSets(*cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, r.layout, 0, 1, &descriptor_sets[i], 0, nullptr);
            vkCmdPushConstants(*cmd, r.layout, push_stages, 0, sizeof p.constants, &p.constants);
            for (const auto& [binding, offset] : p.streams) vkCmdBindVertexBuffers(*cmd, binding, 1, &vb->buffer, &offset);
            vkCmdBindIndexBuffer(*cmd, ib->buffer, p.index_offset, p.index_type);
            const bool query = queries && query_of[i] != ~0u;
            // (precise: increments count samples; a set needs only whether any passed)
            if (query)
                vkCmdBeginQuery(*cmd, queries, query_of[i],
                                !st.visibility_set[0] && d.caps().occlusion_query_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
            vkCmdDrawIndexed(*cmd, p.draw->index_count, p.draw->instances, 0, 0, 0);
            if (query) vkCmdEndQuery(*cmd, queries, query_of[i]);
        }
        r.passes->end(*cmd);
    } else {
        nx::logw(logs::gpu, "{}", begun.error().message);
    }
    if (color) {
        vk::transition(*cmd, *color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyImageToBuffer(*cmd, color->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback->buffer, 1, &copy);
    }
    if (store_depth && depth.image) {
        vk::transition(*cmd, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(*cmd, depth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_buf->buffer, 2, depth_regions);
    }
    VkMemoryBarrier hb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;  // (readbacks; stores)
    hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(*cmd,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, nullptr, 0, nullptr);
    if (auto done = shot.submit_and_wait(); done) {
        if (color) {
            vk::invalidate(d, *readback);
            const u8* src = static_cast<const u8*>(readback->mapped);
            for (u32 y = 0; y < height; ++y)
                std::memcpy(memory().host(cs.data + y * stride * cf.bytes), src + usize{y} * width * cf.bytes, usize{width} * cf.bytes);
        }
        // (the uniform buffers programs stored to)
        bool invalidated = false;
        for (const auto& [address, c] : buffer_copies) {
            if (!c.written || !c.placed || !memory().valid(address, c.bytes, true)) continue;
            if (!invalidated) vk::invalidate(d, *mem_buf), invalidated = true;
            std::memcpy(memory().host(address), static_cast<const u8*>(mem_buf->mapped) + 8 + c.offset, c.bytes);
        }
        if (queries) {
            nx::vector<u64> counts(query_count, 0);
            if (vkGetQueryPoolResults(d.device(), queries, 0, query_count, query_count * 8, counts.data(), 8,
                                      VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS) {
                for (usize i = 0; i < plans.size(); ++i) {
                    if (query_of[i] == ~0u) continue;
                    const DrawState& st = plans[i].draw->state;
                    const u32 at = scene.visibility_buffer + st.visibility_index[0] * 4;
                    if (!memory().valid(at, 4, true)) continue;
                    u32 v;
                    std::memcpy(&v, memory().host(at), 4);
                    const u64 n = counts[query_of[i]];
                    v = st.visibility_set[0] ? (n ? 1u : v) : v + static_cast<u32>(n);
                    std::memcpy(memory().host(at), &v, 4);
                }
            }
        }
        if (store_depth && depth.image) {
            vk::invalidate(d, *depth_buf);
            const u8* src = static_cast<const u8*>(depth_buf->mapped);
            write_depth_surface(ds, dl, width, height, unorm24, reinterpret_cast<const u32*>(src), src + samples * 4);
        }
    } else {
        nx::loge(logs::gpu, "scene: {}", done.error().message);
    }
    if (queries) vkDestroyQueryPool(d.device(), queries, nullptr);
    vkDestroyDescriptorPool(d.device(), pool, nullptr);
    cleanup();
    if (d.validation_errors()) nx::logw(logs::gpu, "{} validation errors so far", d.validation_errors());
}

}  // namespace psvrecore::gxm::hle
