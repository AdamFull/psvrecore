#include "psvrecore/vulkan/resources.hpp"

#include <cstring>

#include "core/foundation/strings/format.h"

namespace psvrecore::vulkan {

VkImageAspectFlags aspect_of(VkFormat format) {
    switch (format) {
        case VK_FORMAT_D16_UNORM:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_IMAGE_ASPECT_DEPTH_BIT;
        case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
        case VK_FORMAT_D16_UNORM_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

u32 texel_size(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_UNORM:
        case VK_FORMAT_R8_UINT:
        case VK_FORMAT_S8_UINT: return 1;
        case VK_FORMAT_R8G8_UNORM:
        case VK_FORMAT_R16_SFLOAT:
        case VK_FORMAT_R16_UINT:
        case VK_FORMAT_R5G6B5_UNORM_PACK16:
        case VK_FORMAT_B5G6R5_UNORM_PACK16:
        case VK_FORMAT_R4G4B4A4_UNORM_PACK16:
        case VK_FORMAT_B4G4R4A4_UNORM_PACK16:
        case VK_FORMAT_A4R4G4B4_UNORM_PACK16:
        case VK_FORMAT_R5G5B5A1_UNORM_PACK16:
        case VK_FORMAT_B5G5R5A1_UNORM_PACK16:
        case VK_FORMAT_A1R5G5B5_UNORM_PACK16:
        case VK_FORMAT_D16_UNORM: return 2;
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        case VK_FORMAT_R16G16_SFLOAT:
        case VK_FORMAT_R16G16_UNORM:
        case VK_FORMAT_R32_SFLOAT:
        case VK_FORMAT_R32_UINT:
        case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        case VK_FORMAT_D32_SFLOAT:
        case VK_FORMAT_D24_UNORM_S8_UINT: return 4;
        case VK_FORMAT_R16G16B16A16_SFLOAT:
        case VK_FORMAT_R16G16B16A16_UNORM:
        case VK_FORMAT_R32G32_SFLOAT:
        case VK_FORMAT_R32G32_UINT: return 8;
        case VK_FORMAT_R32G32B32A32_SFLOAT:
        case VK_FORMAT_R32G32B32A32_UINT: return 16;
        default: return 0;
    }
}

Result<Image> create_image(Device& device, const ImageDesc& desc) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = desc.cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = desc.format;
    info.extent = {desc.width, desc.height, 1};
    info.mipLevels = desc.mips;
    info.arrayLayers = desc.layers;
    info.samples = desc.samples;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = desc.usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    // (transient attachments in lazily allocated memory where there is some: tile-based GPUs keep them on chip)
    if (desc.usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) alloc.usage = VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED;
    Image im;
    im.desc = desc;
    im.aspect = aspect_of(desc.format);
    VkResult r = vmaCreateImage(device.allocator(), &info, &alloc, &im.image, &im.allocation, nullptr);
    if (r != VK_SUCCESS && alloc.usage == VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED) {
        alloc.usage = VMA_MEMORY_USAGE_AUTO;
        r = vmaCreateImage(device.allocator(), &info, &alloc, &im.image, &im.allocation, nullptr);
    }
    if (r != VK_SUCCESS) return nx::err(vk_error("vmaCreateImage", r));
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = im.image;
    view.viewType = desc.cube ? VK_IMAGE_VIEW_TYPE_CUBE : desc.layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    view.format = desc.format;
    view.subresourceRange = {im.aspect, 0, desc.mips, 0, desc.layers};
    if (r = vkCreateImageView(device.device(), &view, nullptr, &im.view); r != VK_SUCCESS) {
        vmaDestroyImage(device.allocator(), im.image, im.allocation);
        return nx::err(vk_error("vkCreateImageView", r));
    }
    return im;
}

void destroy_image(Device& device, Image& image) {
    if (image.view) vkDestroyImageView(device.device(), image.view, nullptr);
    if (image.image) vmaDestroyImage(device.allocator(), image.image, image.allocation);
    image = Image{};
}

namespace {

// The aspect a buffer copy addresses (the depth of a depth/stencil image).
VkImageAspectFlags copy_aspect(const Image& image) {
    return image.aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                                                     : image.aspect;
}

struct Use {
    VkPipelineStageFlags stage;
    VkAccessFlags access;
};

Use use_of(VkImageLayout layout) {
    switch (layout) {
        case VK_IMAGE_LAYOUT_UNDEFINED: return {VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0};
        case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT};
        case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return {VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT};
        case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
            return {VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_ACCESS_SHADER_READ_BIT};
        case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                        VK_ACCESS_INPUT_ATTACHMENT_READ_BIT};
        case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
            return {VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
        case VK_IMAGE_LAYOUT_GENERAL:
            return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
        case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return {VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0};
        default: return {VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
    }
}

}  // namespace

void transition(VkCommandBuffer cmd, Image& image, VkImageLayout to) {
    if (image.layout == to && to != VK_IMAGE_LAYOUT_GENERAL) return;
    const Use from = use_of(image.layout), next = use_of(to);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = from.access;
    b.dstAccessMask = next.access;
    b.oldLayout = image.layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image.image;
    b.subresourceRange = {image.aspect, 0, image.desc.mips, 0, image.desc.layers};
    vkCmdPipelineBarrier(cmd, from.stage, next.stage, 0, 0, nullptr, 0, nullptr, 1, &b);
    image.layout = to;
}

Result<DeviceBuffer> create_device_buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size ? size : 4;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    DeviceBuffer b;
    if (const VkResult r = vmaCreateBuffer(device.allocator(), &info, &alloc, &b.buffer, &b.allocation, nullptr);
        r != VK_SUCCESS)
        return nx::err(vk_error("vmaCreateBuffer", r));
    b.size = info.size;
    return b;
}

void destroy_buffer(Device& device, DeviceBuffer& buffer) {
    if (buffer.buffer) vmaDestroyBuffer(device.allocator(), buffer.buffer, buffer.allocation);
    buffer = DeviceBuffer{};
}

OneShot::OneShot(Device& device) : device_(device) {}

OneShot::~OneShot() {
    if (fence_) vkDestroyFence(device_.device(), fence_, nullptr);
    if (pool_) vkDestroyCommandPool(device_.device(), pool_, nullptr);
}

Result<VkCommandBuffer> OneShot::begin() {
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pi.queueFamilyIndex = device_.queue_family();
    if (const VkResult r = vkCreateCommandPool(device_.device(), &pi, nullptr, &pool_); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateCommandPool", r));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = pool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    if (const VkResult r = vkAllocateCommandBuffers(device_.device(), &ai, &cmd_); r != VK_SUCCESS)
        return nx::err(vk_error("vkAllocateCommandBuffers", r));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &bi);
    return cmd_;
}

Result<void> OneShot::submit_and_wait() {
    if (const VkResult r = vkEndCommandBuffer(cmd_); r != VK_SUCCESS) return nx::err(vk_error("vkEndCommandBuffer", r));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (const VkResult r = vkCreateFence(device_.device(), &fi, nullptr, &fence_); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateFence", r));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    if (const VkResult r = device_.submit(submit, fence_); r != VK_SUCCESS) return nx::err(vk_error("vkQueueSubmit", r));
    if (const VkResult r = vkWaitForFences(device_.device(), 1, &fence_, VK_TRUE, 30'000'000'000ull); r != VK_SUCCESS)
        return nx::err(vk_error("vkWaitForFences", r));
    return {};
}

Result<void> upload_image(Device& device, Image& image, std::span<const u8> texels, u32 layer,
                          VkImageLayout final_layout) {
    auto staging = create_host_buffer(device, texels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!staging) return nx::err(staging.error());
    std::memcpy(staging->mapped, texels.data(), texels.size());
    flush(device, *staging);
    OneShot shot(device);
    auto cmd = shot.begin();
    Result<void> result;
    if (!cmd) {
        result = nx::err(cmd.error());
    } else {
        transition(*cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {copy_aspect(image), 0, layer, 1};
        copy.imageExtent = {image.desc.width, image.desc.height, 1};
        vkCmdCopyBufferToImage(*cmd, staging->buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        transition(*cmd, image, final_layout);
        result = shot.submit_and_wait();
    }
    destroy_buffer(device, *staging);
    return result;
}

Result<nx::vector<u8>> read_image(Device& device, Image& image) {
    const u32 size = texel_size(image.desc.format);
    if (!size) return failure(nx::format("reading images of format {} is not implemented", static_cast<i32>(image.desc.format)));
    if (image.desc.samples != VK_SAMPLE_COUNT_1_BIT) return failure("reading a multisampled image");
    const VkDeviceSize bytes = VkDeviceSize{image.desc.width} * image.desc.height * size;
    auto staging = create_host_buffer(device, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    if (!staging) return nx::err(staging.error());
    OneShot shot(device);
    auto cmd = shot.begin();
    Result<nx::vector<u8>> result = failure("");
    if (!cmd) {
        result = nx::err(cmd.error());
    } else {
        transition(*cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {copy_aspect(image), 0, 0, 1};
        copy.imageExtent = {image.desc.width, image.desc.height, 1};
        vkCmdCopyImageToBuffer(*cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->buffer, 1, &copy);
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(*cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &b, 0, nullptr, 0,
                             nullptr);
        if (auto done = shot.submit_and_wait(); !done) {
            result = nx::err(done.error());
        } else {
            invalidate(device, *staging);
            const u8* p = static_cast<const u8*>(staging->mapped);
            result = nx::vector<u8>(p, p + bytes);
        }
    }
    destroy_buffer(device, *staging);
    return result;
}

Samplers::~Samplers() {
    for (auto& [desc, s] : samplers_) vkDestroySampler(device_.device(), s, nullptr);
}

Result<VkSampler> Samplers::get(const SamplerDesc& d) {
    for (const auto& [desc, s] : samplers_)
        if (desc == d) return s;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info.magFilter = d.mag;
    info.minFilter = d.min;
    info.mipmapMode = d.mip;
    info.addressModeU = d.u;
    info.addressModeV = d.v;
    info.addressModeW = d.w;
    info.mipLodBias = d.lod_bias;
    info.minLod = d.min_lod;
    info.maxLod = d.max_lod;
    info.anisotropyEnable = d.anisotropy > 1.0f && device_.caps().sampler_anisotropy;
    info.maxAnisotropy = nx::min(d.anisotropy, device_.properties().limits.maxSamplerAnisotropy);
    VkSampler s = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateSampler(device_.device(), &info, nullptr, &s); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateSampler", r));
    samplers_.push_back({d, s});
    return s;
}

}  // namespace psvrecore::vulkan
