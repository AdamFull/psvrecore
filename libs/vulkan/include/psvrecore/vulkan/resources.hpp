#pragma once

// Images, samplers, buffers in device memory, and the one-off command buffers that fill and read them.

#include <span>

#include "psvrecore/vulkan/device.hpp"

namespace psvrecore::vulkan {

struct ImageDesc {
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    u32 width = 1, height = 1;
    u32 mips = 1;
    u32 layers = 1;  // (6 for a cube map)
    bool cube = false;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
};

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;  // all mips and layers
    ImageDesc desc;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;  // as recorded so far
};

Result<Image> create_image(Device& device, const ImageDesc& desc);
void destroy_image(Device& device, Image& image);
VkImageAspectFlags aspect_of(VkFormat format);
// Bytes per texel of an uncompressed format (0 for a format this layer does not size).
u32 texel_size(VkFormat format);

// A layout transition of the whole image, with the stages and accesses each layout implies.
void transition(VkCommandBuffer cmd, Image& image, VkImageLayout to);

// A buffer in device-local memory.
struct DeviceBuffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    VkDeviceSize size = 0;
};
Result<DeviceBuffer> create_device_buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage);
void destroy_buffer(Device& device, DeviceBuffer& buffer);

// A command buffer recorded, submitted and waited for (uploads, readbacks, tests); or submitted, and waited for
// later (submit, then done or wait: work the CPU goes on beside). Destroyed, it waits for what it submitted first.
class OneShot {
public:
    explicit OneShot(Device& device);
    ~OneShot();
    OneShot(const OneShot&) = delete;
    OneShot& operator=(const OneShot&) = delete;

    Result<VkCommandBuffer> begin();
    Result<void> submit_and_wait();
    Result<void> submit();
    bool done() const;  // (submitted and finished on the GPU)
    Result<void> wait();

private:
    Device& device_;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkFence fence_ = VK_NULL_HANDLE;
    bool waited_ = false;
};

// Fills mip 0, layer `layer` of an image with tightly packed texels and leaves it in `final_layout`.
Result<void> upload_image(Device& device, Image& image, std::span<const u8> texels, u32 layer = 0,
                          VkImageLayout final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
// Mip 0, layer 0 of a single-sampled colour image, tightly packed (the image is left in TRANSFER_SRC).
Result<nx::vector<u8>> read_image(Device& device, Image& image);

struct SamplerDesc {
    VkFilter mag = VK_FILTER_LINEAR, min = VK_FILTER_LINEAR;
    VkSamplerMipmapMode mip = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    VkSamplerAddressMode u = VK_SAMPLER_ADDRESS_MODE_REPEAT, v = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                         w = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    f32 lod_bias = 0.0f, min_lod = 0.0f, max_lod = VK_LOD_CLAMP_NONE;
    f32 anisotropy = 0.0f;  // (0 or 1: off)
    bool operator==(const SamplerDesc&) const = default;
};

// Samplers by description, made once and kept for the device's life.
class Samplers {
public:
    explicit Samplers(Device& device) : device_(device) {}
    ~Samplers();
    Samplers(const Samplers&) = delete;
    Samplers& operator=(const Samplers&) = delete;
    Result<VkSampler> get(const SamplerDesc& desc);

private:
    Device& device_;
    nx::vector<std::pair<SamplerDesc, VkSampler>> samplers_;
};

}  // namespace psvrecore::vulkan
