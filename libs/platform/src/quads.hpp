#pragma once

// The window's drawing: textured quads into the swapchain's images in one render pass, the frame first (opaque),
// then the overlay's (alpha blended; the on-screen controls). The shaders are made here with gxm's SPIR-V builder.
//
// A quad is a rectangle of the target in pixels, a rectangle of its texture (0 to 1) and a tint the texels are
// multiplied by. No vertex buffer: the vertex shader makes the corners from the vertex index and push constants.

#include <span>

#include "psvrecore/error.hpp"
#include "psvrecore/platform/quad.hpp"
#include "psvrecore/vulkan/device.hpp"
#include "psvrecore/vulkan/resources.hpp"

namespace psvrecore::platform {

nx::vector<u32> quad_vertex_shader();
nx::vector<u32> quad_fragment_shader();

class QuadRenderer {
public:
    explicit QuadRenderer(vulkan::Device& device) : device_(device) {}
    ~QuadRenderer();
    QuadRenderer(const QuadRenderer&) = delete;
    QuadRenderer& operator=(const QuadRenderer&) = delete;

    // For targets of this format (the render pass and pipelines; made again when it changes). `sets` descriptor
    // sets are made, one a texture a frame in flight.
    Result<void> init(VkFormat format, u32 sets);
    VkFormat format() const { return format_; }
    // The swapchain's images (their views and framebuffers); none: forgets them.
    Result<void> set_targets(std::span<const VkImage> images, VkExtent2D extent);

    // A descriptor set (by index, of the `sets`) pointed at a texture.
    VkDescriptorSet texture(u32 set, VkImageView view, bool smooth);

    // Begins the pass on target `image` (its contents cleared to black; left in PRESENT_SRC at the end).
    void begin(VkCommandBuffer cmd, u32 image);
    void draw(VkCommandBuffer cmd, VkDescriptorSet texture, const Quad& q, bool blend);
    void end(VkCommandBuffer cmd);

private:
    void destroy_pipelines();
    void destroy_targets();

    vulkan::Device& device_;
    VkFormat format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{};
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline opaque_ = VK_NULL_HANDLE, blended_ = VK_NULL_HANDLE;
    VkSampler linear_ = VK_NULL_HANDLE, nearest_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    nx::vector<VkDescriptorSet> sets_;
    nx::vector<VkImageView> views_;
    nx::vector<VkFramebuffer> framebuffers_;
};

}  // namespace psvrecore::platform
