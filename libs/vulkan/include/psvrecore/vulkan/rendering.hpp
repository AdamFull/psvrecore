#pragma once

// Drawing into images: one colour attachment (with an optional resolve) and an optional depth/stencil attachment,
// begun with dynamic rendering where the device has it and with a render pass and framebuffer (made once, cached)
// where it does not. A pass whose fragment shaders read the colour attachment (GXM's programmable blending) always
// uses a render pass: the attachment is also an input attachment of the subpass, with a self-dependency, and it
// is cleared with a transfer (it needs VK_IMAGE_USAGE_TRANSFER_DST_BIT) rather than by the load operation.

#include "psvrecore/vulkan/resources.hpp"

namespace psvrecore::vulkan {

struct ColorAttachment {
    Image* image = nullptr;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_STORE;
    VkClearColorValue clear{};
    Image* resolve = nullptr;  // (a multisampled image resolved into this one at the end)
};

struct DepthAttachment {
    Image* image = nullptr;
    VkAttachmentLoadOp load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    VkAttachmentLoadOp stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkAttachmentStoreOp stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    f32 clear_depth = 1.0f;
    u32 clear_stencil = 0;
};

// What a pipeline must be compatible with: the attachments' formats and samples, and whether its fragment shader
// reads the colour attachment.
struct PassFormats {
    VkFormat color = VK_FORMAT_UNDEFINED;
    VkFormat depth = VK_FORMAT_UNDEFINED;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    u32 color_input = 0;  // (a u32: PassFormats is part of hashed keys)
    bool operator==(const PassFormats&) const = default;
};

struct Pass {
    ColorAttachment color;
    DepthAttachment depth;
    VkRect2D area{};  // (0 x 0: the whole colour or depth image)
    bool color_input = false;
    PassFormats formats() const;
};

class RenderPasses {
public:
    explicit RenderPasses(Device& device) : device_(device) {}
    ~RenderPasses();
    RenderPasses(const RenderPasses&) = delete;
    RenderPasses& operator=(const RenderPasses&) = delete;

    // Whether passes with these formats use a render pass (and pipelines need one).
    bool uses_render_pass(const PassFormats& f) const { return f.color_input || !device_.caps().dynamic_rendering; }
    // A render pass pipelines with these formats can be made for (any load and store operations).
    Result<VkRenderPass> compatible(const PassFormats& formats);

    // Moves the attachments to their layouts and begins the pass (the viewport and scissor are the caller's).
    Result<void> begin(VkCommandBuffer cmd, Pass& pass);
    void end(VkCommandBuffer cmd);
    // Drops the framebuffers that use a view (before the view is destroyed).
    void forget(VkImageView view);

private:
    struct PassKey {
        PassFormats formats;
        u32 color_load, color_store, depth_load, depth_store, stencil_load, stencil_store;
        u32 resolve;
        bool operator==(const PassKey&) const = default;
    };
    struct Framebuffer {
        VkRenderPass pass;
        VkImageView views[3];
        u32 width, height;
        VkFramebuffer framebuffer;
    };
    Result<VkRenderPass> render_pass(const PassKey& key);
    Result<VkFramebuffer> framebuffer(VkRenderPass pass, const VkImageView* views, u32 count, u32 width, u32 height);

    Device& device_;
    nx::vector<std::pair<PassKey, VkRenderPass>> passes_;
    nx::vector<Framebuffer> framebuffers_;
    bool dynamic_ = false;  // (the pass begun is dynamic rendering)
};

}  // namespace psvrecore::vulkan
