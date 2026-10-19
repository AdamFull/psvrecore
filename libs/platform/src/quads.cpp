#include "quads.hpp"

#include "psvrecore/gxm/spirv.hpp"
#include "psvrecore/vulkan/pipelines.hpp"
#include "spirv/unified1/spirv.h"

namespace psvrecore::platform {

namespace {

using gxm::spirv::Builder;
using gxm::spirv::Id;

// The push constants: the target rectangle (normalized device coordinates), the texture rectangle, the tint.
struct Constants {
    f32 rect[4];
    f32 uv[4];
    f32 tint[4];
};

// The push constant block, as both stages see it.
Id constants_block(Builder& b, Id v4, Id* variable) {
    const Id block = b.t_struct({v4, v4, v4});
    b.decorate(block, SpvDecorationBlock);
    for (u32 i = 0; i < 3; ++i) b.member_decorate(block, i, SpvDecorationOffset, {i * 16});
    *variable = b.variable(b.t_pointer(SpvStorageClassPushConstant, block), SpvStorageClassPushConstant);
    return block;
}

Id member(Builder& b, Id variable, Id v4, u32 index) {
    const Id ptr = b.op(SpvOpAccessChain, b.t_pointer(SpvStorageClassPushConstant, v4), {variable, b.c_int(static_cast<i32>(index))});
    return b.op(SpvOpLoad, v4, {ptr});
}

}  // namespace

nx::vector<u32> quad_vertex_shader() {
    Builder b;
    b.capability(SpvCapabilityShader);
    const Id f = b.t_float(), i = b.t_int(), v2 = b.t_vector(f, 2), v4 = b.t_vector(f, 4);
    Id pc = 0;
    constants_block(b, v4, &pc);
    const Id index = b.variable(b.t_pointer(SpvStorageClassInput, i), SpvStorageClassInput);
    b.decorate(index, SpvDecorationBuiltIn, {SpvBuiltInVertexIndex});
    const Id position = b.variable(b.t_pointer(SpvStorageClassOutput, v4), SpvStorageClassOutput);
    b.decorate(position, SpvDecorationBuiltIn, {SpvBuiltInPosition});
    const Id uv_out = b.variable(b.t_pointer(SpvStorageClassOutput, v2), SpvStorageClassOutput);
    b.decorate(uv_out, SpvDecorationLocation, {0});

    const Id fn = b.begin_function(b.t_void(), b.t_function(b.t_void()));
    b.block(b.new_label());
    // (the corner: x from bit 0 of the index, y from bit 1, a triangle strip of 4)
    const Id n = b.op(SpvOpLoad, i, {index});
    const Id cx = b.op(SpvOpBitwiseAnd, i, {n, b.c_int(1)});
    const Id cy = b.op(SpvOpBitwiseAnd, i, {b.op(SpvOpShiftRightArithmetic, i, {n, b.c_int(1)}), b.c_int(1)});
    const Id t = b.op(SpvOpCompositeConstruct, v2, {b.op(SpvOpConvertSToF, f, {cx}), b.op(SpvOpConvertSToF, f, {cy})});
    auto corner = [&](Id rect) {
        const Id lo = b.op(SpvOpVectorShuffle, v2, {rect, rect, 0, 1});
        const Id hi = b.op(SpvOpVectorShuffle, v2, {rect, rect, 2, 3});
        return b.ext(v2, 46 /* FMix */, {lo, hi, t});
    };
    const Id xy = corner(member(b, pc, v4, 0));
    b.op0(SpvOpStore, {position, b.op(SpvOpCompositeConstruct, v4, {xy, b.c_float(0.0f), b.c_float(1.0f)})});
    b.op0(SpvOpStore, {uv_out, corner(member(b, pc, v4, 1))});
    b.op0(SpvOpReturn, {});
    b.end_function();
    const Id interface[] = {index, position, uv_out};
    b.entry_point(SpvExecutionModelVertex, fn, "main", interface);
    return b.module();
}

nx::vector<u32> quad_fragment_shader() {
    Builder b;
    b.capability(SpvCapabilityShader);
    const Id f = b.t_float(), v2 = b.t_vector(f, 2), v4 = b.t_vector(f, 4);
    Id pc = 0;
    constants_block(b, v4, &pc);
    const Id sampled = b.t_sampled_image(b.t_image(f, SpvDim2D, 0, 0, 0, 1, SpvImageFormatUnknown));
    const Id texture = b.variable(b.t_pointer(SpvStorageClassUniformConstant, sampled), SpvStorageClassUniformConstant);
    b.decorate(texture, SpvDecorationDescriptorSet, {0});
    b.decorate(texture, SpvDecorationBinding, {0});
    const Id uv_in = b.variable(b.t_pointer(SpvStorageClassInput, v2), SpvStorageClassInput);
    b.decorate(uv_in, SpvDecorationLocation, {0});
    const Id color = b.variable(b.t_pointer(SpvStorageClassOutput, v4), SpvStorageClassOutput);
    b.decorate(color, SpvDecorationLocation, {0});

    const Id fn = b.begin_function(b.t_void(), b.t_function(b.t_void()));
    b.block(b.new_label());
    const Id texel = b.op(SpvOpImageSampleImplicitLod, v4, {b.op(SpvOpLoad, sampled, {texture}), b.op(SpvOpLoad, v2, {uv_in})});
    b.op0(SpvOpStore, {color, b.op(SpvOpFMul, v4, {texel, member(b, pc, v4, 2)})});
    b.op0(SpvOpReturn, {});
    b.end_function();
    const Id interface[] = {uv_in, color};
    b.entry_point(SpvExecutionModelFragment, fn, "main", interface);
    b.execution_mode(fn, SpvExecutionModeOriginUpperLeft);
    return b.module();
}

QuadRenderer::~QuadRenderer() {
    destroy_targets();
    destroy_pipelines();
    const VkDevice d = device_.device();
    if (pool_) vkDestroyDescriptorPool(d, pool_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(d, set_layout_, nullptr);
    if (layout_) vkDestroyPipelineLayout(d, layout_, nullptr);
    if (linear_) vkDestroySampler(d, linear_, nullptr);
    if (nearest_) vkDestroySampler(d, nearest_, nullptr);
}

void QuadRenderer::destroy_pipelines() {
    const VkDevice d = device_.device();
    if (opaque_) vkDestroyPipeline(d, opaque_, nullptr);
    if (blended_) vkDestroyPipeline(d, blended_, nullptr);
    if (pass_) vkDestroyRenderPass(d, pass_, nullptr);
    opaque_ = blended_ = VK_NULL_HANDLE;
    pass_ = VK_NULL_HANDLE;
}

void QuadRenderer::destroy_targets() {
    const VkDevice d = device_.device();
    for (VkFramebuffer fb : framebuffers_) vkDestroyFramebuffer(d, fb, nullptr);
    for (VkImageView v : views_) vkDestroyImageView(d, v, nullptr);
    framebuffers_.clear();
    views_.clear();
}

Result<void> QuadRenderer::init(VkFormat format, u32 sets) {
    const VkDevice d = device_.device();
    if (!layout_) {
        // (once: the samplers, the descriptor set layout and pool, the pipeline layout)
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.maxLod = 0.0f;
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        if (VkResult r = vkCreateSampler(d, &si, nullptr, &linear_); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreateSampler", r));
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        if (VkResult r = vkCreateSampler(d, &si, nullptr, &nearest_); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreateSampler", r));
        VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
                                             nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &binding;
        if (VkResult r = vkCreateDescriptorSetLayout(d, &li, nullptr, &set_layout_); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreateDescriptorSetLayout", r));
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Constants)};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout_;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &push;
        if (VkResult r = vkCreatePipelineLayout(d, &pli, nullptr, &layout_); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreatePipelineLayout", r));
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = sets;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        if (VkResult r = vkCreateDescriptorPool(d, &pi, nullptr, &pool_); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreateDescriptorPool", r));
        nx::vector<VkDescriptorSetLayout> layouts(sets, set_layout_);
        sets_.resize(sets);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool_;
        ai.descriptorSetCount = sets;
        ai.pSetLayouts = layouts.data();
        if (VkResult r = vkAllocateDescriptorSets(d, &ai, sets_.data()); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkAllocateDescriptorSets", r));
    }
    if (format == format_ && pass_) return {};
    destroy_targets();
    destroy_pipelines();
    format_ = format;

    // The pass: one colour attachment, cleared, presented after.
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    const VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &ref;
    // (the image's acquisition is waited for at the colour output stage: the layout change waits for it too)
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &attachment;
    rp.subpassCount = 1;
    rp.pSubpasses = &subpass;
    rp.dependencyCount = 1;
    rp.pDependencies = &dependency;
    if (VkResult r = vkCreateRenderPass(d, &rp, nullptr, &pass_); r != VK_SUCCESS)
        return nx::err(vulkan::vk_error("vkCreateRenderPass", r));

    // The pipelines: a strip of 4 corners, viewport and scissor dynamic, blending on or off.
    const nx::vector<u32> vs_words = quad_vertex_shader(), fs_words = quad_fragment_shader();
    auto vs = vulkan::create_shader(device_, vs_words);
    if (!vs) return nx::err(vs.error());
    auto fs = vulkan::create_shader(device_, fs_words);
    if (!fs) {
        vkDestroyShaderModule(d, *vs, nullptr);
        return nx::err(fs.error());
    }
    VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                              {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = *vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = *fs;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                           VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &blend;
    const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = layout_;
    gp.renderPass = pass_;
    VkResult r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &opaque_);
    if (r == VK_SUCCESS) {
        // (over what is there: straight alpha)
        blend.blendEnable = VK_TRUE;
        blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp = VK_BLEND_OP_ADD;
        blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.alphaBlendOp = VK_BLEND_OP_ADD;
        r = vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &blended_);
    }
    vkDestroyShaderModule(d, *vs, nullptr);
    vkDestroyShaderModule(d, *fs, nullptr);
    if (r != VK_SUCCESS) return nx::err(vulkan::vk_error("vkCreateGraphicsPipelines", r));
    return {};
}

Result<void> QuadRenderer::set_targets(std::span<const VkImage> images, VkExtent2D extent) {
    destroy_targets();
    extent_ = extent;
    const VkDevice d = device_.device();
    for (VkImage image : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        if (VkResult r = vkCreateImageView(d, &vi, nullptr, &view); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreateImageView", r));
        views_.push_back(view);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = pass_;
        fi.attachmentCount = 1;
        fi.pAttachments = &view;
        fi.width = extent.width;
        fi.height = extent.height;
        fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (VkResult r = vkCreateFramebuffer(d, &fi, nullptr, &fb); r != VK_SUCCESS)
            return nx::err(vulkan::vk_error("vkCreateFramebuffer", r));
        framebuffers_.push_back(fb);
    }
    return {};
}

VkDescriptorSet QuadRenderer::texture(u32 set, VkImageView view, bool smooth) {
    const VkDescriptorImageInfo image{smooth ? linear_ : nearest_, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = sets_[set];
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &image;
    vkUpdateDescriptorSets(device_.device(), 1, &w, 0, nullptr);
    return sets_[set];
}

void QuadRenderer::begin(VkCommandBuffer cmd, u32 image) {
    VkClearValue black{};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = pass_;
    bi.framebuffer = framebuffers_[image];
    bi.renderArea = {{0, 0}, extent_};
    bi.clearValueCount = 1;
    bi.pClearValues = &black;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    const VkViewport viewport{0, 0, static_cast<f32>(extent_.width), static_cast<f32>(extent_.height), 0, 1};
    const VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

void QuadRenderer::draw(VkCommandBuffer cmd, VkDescriptorSet texture, const Quad& q, bool blend) {
    const f32 w = static_cast<f32>(extent_.width), h = static_cast<f32>(extent_.height);
    const Constants c{{q.x0 / w * 2 - 1, q.y0 / h * 2 - 1, q.x1 / w * 2 - 1, q.y1 / h * 2 - 1},
                      {q.u0, q.v0, q.u1, q.v1},
                      {q.r, q.g, q.b, q.a}};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, blend ? blended_ : opaque_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &texture, 0, nullptr);
    vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof c, &c);
    vkCmdDraw(cmd, 4, 1, 0, 0);
}

void QuadRenderer::end(VkCommandBuffer cmd) { vkCmdEndRenderPass(cmd); }

}  // namespace psvrecore::platform
