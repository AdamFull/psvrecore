#include "psvrecore/vulkan/pipelines.hpp"

#include <cstring>
#include <type_traits>

#include "psvrecore/io/file.hpp"
#include "core/foundation/core/hash.h"

namespace psvrecore::vulkan {

static_assert(std::has_unique_object_representations_v<GraphicsPipelineDesc>,
              "GraphicsPipelineDesc is hashed as bytes: it must have no padding");

GraphicsPipelineDesc GraphicsPipelineDesc::defaults() {
    GraphicsPipelineDesc d;
    std::memset(&d, 0, sizeof d);
    d.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    d.polygon = VK_POLYGON_MODE_FILL;
    d.cull = VK_CULL_MODE_NONE;
    d.front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    d.depth_compare = VK_COMPARE_OP_ALWAYS;
    d.blend.src_color = d.blend.src_alpha = VK_BLEND_FACTOR_ONE;
    d.blend.dst_color = d.blend.dst_alpha = VK_BLEND_FACTOR_ZERO;
    d.blend.color_op = d.blend.alpha_op = VK_BLEND_OP_ADD;
    d.blend.write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    d.blend.logic_op = VK_LOGIC_OP_COPY;
    d.stencil_front.compareOp = d.stencil_back.compareOp = VK_COMPARE_OP_ALWAYS;
    d.stencil_front.failOp = d.stencil_front.passOp = d.stencil_front.depthFailOp = VK_STENCIL_OP_KEEP;
    d.stencil_back = d.stencil_front;
    d.formats = PassFormats{};
    return d;
}

u64 GraphicsPipelineDesc::hash() const { return nx::hash_fnv1a64(reinterpret_cast<const char*>(this), sizeof *this); }

bool GraphicsPipelineDesc::same(const GraphicsPipelineDesc& other) const {
    return std::memcmp(this, &other, sizeof *this) == 0;
}

Result<VkShaderModule> create_shader(Device& device, std::span<const u32> spirv) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = spirv.size() * 4;
    info.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateShaderModule(device.device(), &info, nullptr, &m); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateShaderModule", r));
    return m;
}

Pipelines::Pipelines(Device& device, RenderPasses& passes) : device_(device), passes_(passes) {
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    vkCreatePipelineCache(device_.device(), &info, nullptr, &cache_);
}

Pipelines::~Pipelines() {
    for (auto& [hash, list] : pipelines_)
        for (auto& [desc, p] : list) vkDestroyPipeline(device_.device(), p, nullptr);
    if (cache_) vkDestroyPipelineCache(device_.device(), cache_, nullptr);
}

Result<VkPipeline> Pipelines::graphics(const GraphicsPipelineDesc& d) {
    const u64 h = d.hash();
    auto& list = pipelines_[h];
    for (const auto& [desc, p] : list)
        if (desc.same(d)) return p;

    VkSpecializationMapEntry entries[GraphicsPipelineDesc::kMaxSpecialization];
    for (u32 i = 0; i < d.specialization_count; ++i) entries[i] = {i, i * 4, 4};
    VkSpecializationInfo spec{d.specialization_count, entries, d.specialization_count * 4u, d.specialization};
    VkPipelineShaderStageCreateInfo stages[2]{};
    u32 stage_count = 0;
    for (const auto& [module, stage] : {std::pair{d.vertex, VK_SHADER_STAGE_VERTEX_BIT},
                                        std::pair{d.fragment, VK_SHADER_STAGE_FRAGMENT_BIT}}) {
        if (!module) continue;
        VkPipelineShaderStageCreateInfo& s = stages[stage_count++];
        s.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        s.stage = stage;
        s.module = module;
        s.pName = "main";
        s.pSpecializationInfo = d.specialization_count ? &spec : nullptr;
    }

    VkVertexInputBindingDescription bindings[GraphicsPipelineDesc::kMaxBindings];
    for (u32 i = 0; i < d.binding_count; ++i)
        bindings[i] = {i, d.bindings[i].stride,
                       d.bindings[i].per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[GraphicsPipelineDesc::kMaxAttributes];
    for (u32 i = 0; i < d.attribute_count; ++i)
        attributes[i] = {d.attributes[i].location, d.attributes[i].binding, d.attributes[i].format, d.attributes[i].offset};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = d.binding_count;
    vertex.pVertexBindingDescriptions = bindings;
    vertex.vertexAttributeDescriptionCount = d.attribute_count;
    vertex.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = d.topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = d.depth_clamp && device_.caps().depth_clamp;
    raster.polygonMode = d.polygon;
    raster.cullMode = d.cull;
    raster.frontFace = d.front;
    raster.depthBiasEnable = d.depth_bias ? VK_TRUE : VK_FALSE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.formats.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = d.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = d.depth_compare;
    ds.stencilTestEnable = d.stencil_test ? VK_TRUE : VK_FALSE;
    ds.front = d.stencil_front;
    ds.back = d.stencil_back;
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = d.blend.enable ? VK_TRUE : VK_FALSE;
    attachment.srcColorBlendFactor = d.blend.src_color;
    attachment.dstColorBlendFactor = d.blend.dst_color;
    attachment.colorBlendOp = d.blend.color_op;
    attachment.srcAlphaBlendFactor = d.blend.src_alpha;
    attachment.dstAlphaBlendFactor = d.blend.dst_alpha;
    attachment.alphaBlendOp = d.blend.alpha_op;
    attachment.colorWriteMask = d.blend.write_mask;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.logicOpEnable = d.blend.logic_enable && device_.caps().logic_op;
    blend.logicOp = d.blend.logic_op;
    const bool has_color = d.formats.color != VK_FORMAT_UNDEFINED;
    blend.attachmentCount = has_color ? 1 : 0;
    blend.pAttachments = &attachment;
    if (d.formats.color_input && device_.caps().raster_order_attachment)
        blend.flags |= VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
                                             VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                             VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                             VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<u32>(std::size(dynamic_states));
    dynamic.pDynamicStates = dynamic_states;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    const VkFormat color_format = d.formats.color;
    if (passes_.uses_render_pass(d.formats)) {
        auto rp = passes_.compatible(d.formats);
        if (!rp) return nx::err(rp.error());
        info.renderPass = *rp;
    } else {
        rendering.colorAttachmentCount = has_color ? 1 : 0;
        rendering.pColorAttachmentFormats = &color_format;
        rendering.depthAttachmentFormat = d.formats.depth;
        if (aspect_of(d.formats.depth) & VK_IMAGE_ASPECT_STENCIL_BIT) rendering.stencilAttachmentFormat = d.formats.depth;
        info.pNext = &rendering;
    }
    info.stageCount = stage_count;
    info.pStages = stages;
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = d.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateGraphicsPipelines(device_.device(), cache_, 1, &info, nullptr, &p); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateGraphicsPipelines", r));
    list.push_back({d, p});
    ++count_;
    return p;
}

Result<void> Pipelines::save_cache(nx::fs::path_view path) const {
    usize size = 0;
    if (const VkResult r = vkGetPipelineCacheData(device_.device(), cache_, &size, nullptr); r != VK_SUCCESS)
        return nx::err(vk_error("vkGetPipelineCacheData", r));
    nx::vector<u8> data(size);
    if (const VkResult r = vkGetPipelineCacheData(device_.device(), cache_, &size, data.data()); r != VK_SUCCESS)
        return nx::err(vk_error("vkGetPipelineCacheData", r));
    data.resize(size);
    return io::write_file(path, data);
}

Result<void> Pipelines::load_cache(nx::fs::path_view path) {
    const auto data = io::read_file(path);
    if (!data) return nx::err(data.error());
    // (the header names the vendor, device and cache UUID; a cache from another driver would only be ignored, but
    // checking it here keeps that from being silent)
    const VkPhysicalDeviceProperties& p = device_.properties();
    if (data->size() < 32) return failure("pipeline cache: too short");
    u32 vendor, device_id;
    std::memcpy(&vendor, data->data() + 8, 4);
    std::memcpy(&device_id, data->data() + 12, 4);
    if (vendor != p.vendorID || device_id != p.deviceID || std::memcmp(data->data() + 16, p.pipelineCacheUUID, 16) != 0)
        return failure("pipeline cache: from another device or driver");
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data->size();
    info.pInitialData = data->data();
    VkPipelineCache loaded = VK_NULL_HANDLE;
    if (const VkResult r = vkCreatePipelineCache(device_.device(), &info, nullptr, &loaded); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreatePipelineCache", r));
    if (cache_) vkMergePipelineCaches(device_.device(), loaded, 1, &cache_), vkDestroyPipelineCache(device_.device(), cache_, nullptr);
    cache_ = loaded;
    return {};
}

}  // namespace psvrecore::vulkan
