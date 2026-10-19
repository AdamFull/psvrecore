#include "psvrecore/vulkan/rendering.hpp"

#include <cstring>

namespace psvrecore::vulkan {

PassFormats Pass::formats() const {
    PassFormats f;
    if (color.image) f.color = color.image->desc.format, f.samples = color.image->desc.samples;
    if (depth.image) f.depth = depth.image->desc.format, f.samples = depth.image->desc.samples;
    f.color_input = color_input ? 1 : 0;
    return f;
}

RenderPasses::~RenderPasses() {
    for (const Framebuffer& f : framebuffers_) vkDestroyFramebuffer(device_.device(), f.framebuffer, nullptr);
    for (const auto& [key, pass] : passes_) vkDestroyRenderPass(device_.device(), pass, nullptr);
}

Result<VkRenderPass> RenderPasses::compatible(const PassFormats& formats) {
    PassKey key{};
    key.formats = formats;
    key.color_load = VK_ATTACHMENT_LOAD_OP_LOAD;
    key.color_store = VK_ATTACHMENT_STORE_OP_STORE;
    key.depth_load = key.stencil_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
    key.depth_store = key.stencil_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    return render_pass(key);
}

Result<VkRenderPass> RenderPasses::render_pass(const PassKey& key) {
    for (const auto& [k, pass] : passes_)
        if (k == key) return pass;
    const PassFormats& f = key.formats;
    // In a pass that reads its colour attachment, the attachment is in the general layout (a feedback loop).
    const VkImageLayout color_layout =
        f.color_input ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentDescription attachments[3]{};
    u32 count = 0;
    VkAttachmentReference color_ref{VK_ATTACHMENT_UNUSED, color_layout}, depth_ref{VK_ATTACHMENT_UNUSED},
        resolve_ref{VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    if (f.color != VK_FORMAT_UNDEFINED) {
        VkAttachmentDescription& a = attachments[count];
        a.format = f.color;
        a.samples = f.samples;
        a.loadOp = static_cast<VkAttachmentLoadOp>(key.color_load);
        a.storeOp = static_cast<VkAttachmentStoreOp>(key.color_store);
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = color_layout;
        color_ref.attachment = count++;
    }
    if (f.depth != VK_FORMAT_UNDEFINED) {
        VkAttachmentDescription& a = attachments[count];
        a.format = f.depth;
        a.samples = f.samples;
        a.loadOp = static_cast<VkAttachmentLoadOp>(key.depth_load);
        a.storeOp = static_cast<VkAttachmentStoreOp>(key.depth_store);
        a.stencilLoadOp = static_cast<VkAttachmentLoadOp>(key.stencil_load);
        a.stencilStoreOp = static_cast<VkAttachmentStoreOp>(key.stencil_store);
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref = {count++, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    }
    if (key.resolve) {
        VkAttachmentDescription& a = attachments[count];
        a.format = f.color;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        resolve_ref.attachment = count++;
    }
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    if (color_ref.attachment != VK_ATTACHMENT_UNUSED) {
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_ref;
        if (key.resolve) subpass.pResolveAttachments = &resolve_ref;
    }
    if (depth_ref.attachment != VK_ATTACHMENT_UNUSED) subpass.pDepthStencilAttachment = &depth_ref;
    if (f.color_input) {
        subpass.inputAttachmentCount = 1;
        subpass.pInputAttachments = &color_ref;
        if (device_.caps().raster_order_attachment)
            subpass.flags |= VK_SUBPASS_DESCRIPTION_RASTERIZATION_ORDER_ATTACHMENT_COLOR_ACCESS_BIT_EXT;
    }
    // Before the pass: whatever came before it (layouts are set by explicit barriers). Within it, for a pass that
    // reads its colour: each draw's writes before the next one's reads (by region, a barrier between draws).
    VkSubpassDependency deps[2]{};
    u32 dep_count = 0;
    {
        VkSubpassDependency& d = deps[dep_count++];
        d.srcSubpass = VK_SUBPASS_EXTERNAL;
        d.dstSubpass = 0;
        d.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        d.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        d.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        d.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    }
    if (f.color_input) {
        VkSubpassDependency& d = deps[dep_count++];
        d.srcSubpass = d.dstSubpass = 0;
        d.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        d.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        d.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        d.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        d.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    }
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = dep_count;
    info.pDependencies = deps;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateRenderPass(device_.device(), &info, nullptr, &pass); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateRenderPass", r));
    passes_.push_back({key, pass});
    return pass;
}

Result<VkFramebuffer> RenderPasses::framebuffer(VkRenderPass pass, const VkImageView* views, u32 count, u32 width,
                                                u32 height) {
    for (const Framebuffer& f : framebuffers_) {
        if (f.pass != pass || f.width != width || f.height != height) continue;
        bool same = true;
        for (u32 i = 0; i < 3; ++i) same &= f.views[i] == (i < count ? views[i] : VK_NULL_HANDLE);
        if (same) return f.framebuffer;
    }
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = pass;
    info.attachmentCount = count;
    info.pAttachments = views;
    info.width = width;
    info.height = height;
    info.layers = 1;
    Framebuffer f{pass, {}, width, height, VK_NULL_HANDLE};
    for (u32 i = 0; i < count; ++i) f.views[i] = views[i];
    if (const VkResult r = vkCreateFramebuffer(device_.device(), &info, nullptr, &f.framebuffer); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateFramebuffer", r));
    framebuffers_.push_back(f);
    return f.framebuffer;
}

void RenderPasses::forget(VkImageView view) {
    for (usize i = framebuffers_.size(); i-- > 0;) {
        const Framebuffer& f = framebuffers_[i];
        if (f.views[0] == view || f.views[1] == view || f.views[2] == view) {
            vkDestroyFramebuffer(device_.device(), f.framebuffer, nullptr);
            framebuffers_.erase(framebuffers_.begin() + static_cast<isize>(i));
        }
    }
}

Result<void> RenderPasses::begin(VkCommandBuffer cmd, Pass& pass) {
    const PassFormats f = pass.formats();
    Image* size_of = pass.color.image ? pass.color.image : pass.depth.image;
    if (!size_of) return failure("a pass without attachments");
    VkRect2D area = pass.area;
    if (!area.extent.width || !area.extent.height) area = {{0, 0}, {size_of->desc.width, size_of->desc.height}};
    const VkImageLayout color_layout = f.color_input ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // A colour attachment the shaders read is cleared before the pass and loaded: an Intel Arc driver reads wrong
    // values back from an attachment cleared by its load operation (see KNOWN_ISSUES).
    VkAttachmentLoadOp color_load = pass.color.load;
    if (f.color_input && pass.color.image && color_load == VK_ATTACHMENT_LOAD_OP_CLEAR) {
        transition(cmd, *pass.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, pass.color.image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &pass.color.clear, 1,
                             &range);
        color_load = VK_ATTACHMENT_LOAD_OP_LOAD;
    }
    if (pass.color.image) transition(cmd, *pass.color.image, color_layout);
    if (pass.color.resolve) transition(cmd, *pass.color.resolve, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    if (pass.depth.image) transition(cmd, *pass.depth.image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    if (!uses_render_pass(f)) {
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = area;
        info.layerCount = 1;
        if (pass.color.image) {
            color.imageView = pass.color.image->view;
            color.imageLayout = color_layout;
            color.loadOp = color_load;
            color.storeOp = pass.color.store;
            color.clearValue.color = pass.color.clear;
            if (pass.color.resolve) {
                color.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
                color.resolveImageView = pass.color.resolve->view;
                color.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            }
            info.colorAttachmentCount = 1;
            info.pColorAttachments = &color;
        }
        if (pass.depth.image) {
            depth.imageView = pass.depth.image->view;
            depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depth.loadOp = pass.depth.load;
            depth.storeOp = pass.depth.store;
            depth.clearValue.depthStencil = {pass.depth.clear_depth, pass.depth.clear_stencil};
            info.pDepthAttachment = &depth;
            if (pass.depth.image->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) {
                stencil = depth;
                stencil.loadOp = pass.depth.stencil_load;
                stencil.storeOp = pass.depth.stencil_store;
                info.pStencilAttachment = &stencil;
            }
        }
        device_.begin_rendering(cmd, &info);
        dynamic_ = true;
        return {};
    }

    PassKey key{};
    key.formats = f;
    key.color_load = color_load;
    key.color_store = pass.color.store;
    key.depth_load = pass.depth.load;
    key.depth_store = pass.depth.store;
    key.stencil_load = pass.depth.stencil_load;
    key.stencil_store = pass.depth.stencil_store;
    key.resolve = pass.color.resolve ? 1 : 0;
    auto rp = render_pass(key);
    if (!rp) return nx::err(rp.error());
    VkImageView views[3];
    VkClearValue clears[3]{};
    u32 count = 0;
    if (pass.color.image) clears[count].color = pass.color.clear, views[count++] = pass.color.image->view;
    if (pass.depth.image) {
        clears[count].depthStencil = {pass.depth.clear_depth, pass.depth.clear_stencil};
        views[count++] = pass.depth.image->view;
    }
    if (pass.color.resolve) views[count++] = pass.color.resolve->view;
    auto fb = framebuffer(*rp, views, count, size_of->desc.width, size_of->desc.height);
    if (!fb) return nx::err(fb.error());
    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = *rp;
    info.framebuffer = *fb;
    info.renderArea = area;
    info.clearValueCount = count;
    info.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    dynamic_ = false;
    return {};
}

void RenderPasses::end(VkCommandBuffer cmd) {
    if (dynamic_) device_.end_rendering(cmd);
    else vkCmdEndRenderPass(cmd);
}

}  // namespace psvrecore::vulkan
