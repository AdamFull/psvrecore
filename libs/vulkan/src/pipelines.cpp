#include "psvrecore/vulkan/pipelines.hpp"

#include <bit>
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

// (eight bytes at a time: a description is some 500 bytes, looked up for each draw of each scene; byte by byte, the
// lookups took a twentieth of the thread that ends the game's scenes on a phone)
u64 GraphicsPipelineDesc::hash() const {
    constexpr u64 k1 = 0x9E3779B97F4A7C15ull, k2 = 0xC2B2AE3D27D4EB4Full;
    const auto* p = reinterpret_cast<const unsigned char*>(this);
    u64 h = k1 ^ sizeof *this;
    usize i = 0;
    for (; i + 8 <= sizeof *this; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h = std::rotl(h ^ (w * k2), 31) * k1;
    }
    for (; i < sizeof *this; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    return h ^ (h >> 33);
}

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
        for (Entry& e : list) vkDestroyPipeline(device_.device(), e.pipeline, nullptr);
    for (VkPipeline p : replaced_) vkDestroyPipeline(device_.device(), p, nullptr);
    for (auto& parts : libraries_)
        for (auto& [hash, list] : parts)
            for (auto& [desc, p] : list) vkDestroyPipeline(device_.device(), p, nullptr);
    if (cache_) vkDestroyPipelineCache(device_.device(), cache_, nullptr);
}

const Pipelines::Entry* Pipelines::find_locked(const GraphicsPipelineDesc& d) const {
    if (auto it = pipelines_.find(d.hash()); it != pipelines_.end())
        for (const Entry& e : it->second)
            if (e.desc.same(d)) return &e;
    return nullptr;
}

VkPipeline Pipelines::find(const GraphicsPipelineDesc& d, bool* linked) const {
    nx::scoped_lock<nx::mutex> held(lock_);
    const Entry* e = find_locked(d);
    if (linked) *linked = e && e->linked;
    return e ? e->pipeline : VK_NULL_HANDLE;
}

Result<VkPipeline> Pipelines::graphics(const GraphicsPipelineDesc& d) {
    VkRenderPass render_pass = VK_NULL_HANDLE;
    {
        nx::scoped_lock<nx::mutex> held(lock_);
        if (const Entry* e = find_locked(d); e && !e->linked) return e->pipeline;
        if (passes_.uses_render_pass(d.formats)) {
            auto rp = passes_.compatible(d.formats);
            if (!rp) return nx::err(rp.error());
            render_pass = *rp;
        }
    }

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
    if (render_pass) {
        info.renderPass = render_pass;
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
    nx::scoped_lock<nx::mutex> held(lock_);
    if (const Entry* e = find_locked(d)) {
        Entry& found = const_cast<Entry&>(*e);
        // (another thread made the same one meanwhile: its kept, this one dropped)
        if (!found.linked) {
            vkDestroyPipeline(device_.device(), p, nullptr);
            return found.pipeline;
        }
        // (in the place of the linked one)
        replaced_.push_back(found.pipeline);
        found.pipeline = p, found.linked = false;
        return p;
    }
    pipelines_[d.hash()].push_back({d, p, false});
    count_.fetch_add(1, std::memory_order_relaxed);
    return p;
}

namespace {

enum Part : u32 { kVertexInput, kPreRasterization, kFragmentShader, kFragmentOutput };

// (a part's description: what its library reads of the pipeline's, the rest left as defaults() has it; the state
// made dynamic left out)
GraphicsPipelineDesc part_of(u32 part, const GraphicsPipelineDesc& d, bool dynamic) {
    GraphicsPipelineDesc p = GraphicsPipelineDesc::defaults();
    switch (part) {
        case kVertexInput:
            std::memcpy(p.bindings, d.bindings, sizeof p.bindings);
            std::memcpy(p.attributes, d.attributes, sizeof p.attributes);
            p.binding_count = d.binding_count, p.attribute_count = d.attribute_count, p.topology = d.topology;
            break;
        case kPreRasterization:
            p.vertex = d.vertex, p.layout = d.layout, p.polygon = d.polygon, p.depth_clamp = d.depth_clamp;
            if (!dynamic) p.cull = d.cull, p.front = d.front, p.depth_bias = d.depth_bias;
            std::memcpy(p.specialization, d.specialization, sizeof p.specialization);
            p.specialization_count = d.specialization_count;
            p.formats = d.formats;
            break;
        case kFragmentShader:
            p.fragment = d.fragment, p.layout = d.layout;
            if (!dynamic) {
                p.depth_test = d.depth_test, p.depth_write = d.depth_write, p.depth_compare = d.depth_compare;
                p.stencil_test = d.stencil_test, p.stencil_front = d.stencil_front, p.stencil_back = d.stencil_back;
            }
            std::memcpy(p.specialization, d.specialization, sizeof p.specialization);
            p.specialization_count = d.specialization_count;
            p.formats = d.formats;
            break;
        default:
            p.blend = d.blend, p.formats = d.formats;
            break;
    }
    return p;
}

// (a part's dynamic states: the first three, and the rest with dynamic_state)
template <usize N>
u32 dynamic_state_count(const VkDynamicState (&states)[N], bool dynamic) {
    return dynamic ? static_cast<u32>(N) : 3u;
}

}  // namespace

void set_dynamic_state(VkCommandBuffer cmd, const GraphicsPipelineDesc& d) {
    vkCmdSetCullMode(cmd, d.cull);
    vkCmdSetFrontFace(cmd, d.front);
    vkCmdSetDepthBiasEnable(cmd, d.depth_bias ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthTestEnable(cmd, d.depth_test ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthWriteEnable(cmd, d.depth_write ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cmd, d.depth_compare);
    vkCmdSetStencilTestEnable(cmd, d.stencil_test ? VK_TRUE : VK_FALSE);
    const VkStencilOpState& f = d.stencil_front;
    const VkStencilOpState& b = d.stencil_back;
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, f.failOp, f.passOp, f.depthFailOp, f.compareOp);
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, b.failOp, b.passOp, b.depthFailOp, b.compareOp);
}

Result<VkPipeline> Pipelines::library(u32 part, const GraphicsPipelineDesc& full) {
    const bool dynamic_states = device_.caps().dynamic_state;
    const GraphicsPipelineDesc d = part_of(part, full, dynamic_states);
    const u64 hash = d.hash();
    {
        nx::scoped_lock<nx::mutex> held(lock_);
        if (auto it = libraries_[part].find(hash); it != libraries_[part].end())
            for (const auto& [desc, p] : it->second)
                if (desc.same(d)) return p;
    }
    VkSpecializationMapEntry entries[GraphicsPipelineDesc::kMaxSpecialization];
    for (u32 i = 0; i < d.specialization_count; ++i) entries[i] = {i, i * 4, 4};
    VkSpecializationInfo spec{d.specialization_count, entries, d.specialization_count * 4u, d.specialization};
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = part == kPreRasterization ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    stage.module = part == kPreRasterization ? d.vertex : d.fragment;
    stage.pName = "main";
    stage.pSpecializationInfo = d.specialization_count ? &spec : nullptr;
    VkVertexInputBindingDescription bindings[GraphicsPipelineDesc::kMaxBindings];
    for (u32 i = 0; i < d.binding_count; ++i)
        bindings[i] = {i, d.bindings[i].stride, d.bindings[i].per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[GraphicsPipelineDesc::kMaxAttributes];
    for (u32 i = 0; i < d.attribute_count; ++i)
        attributes[i] = {d.attributes[i].location, d.attributes[i].binding, d.attributes[i].format, d.attributes[i].offset};
    VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertex.vertexBindingDescriptionCount = d.binding_count, vertex.pVertexBindingDescriptions = bindings;
    vertex.vertexAttributeDescriptionCount = d.attribute_count, vertex.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = d.topology;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1, viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = d.depth_clamp && device_.caps().depth_clamp;
    raster.polygonMode = d.polygon, raster.cullMode = d.cull, raster.frontFace = d.front, raster.lineWidth = 1.0f;
    raster.depthBiasEnable = d.depth_bias ? VK_TRUE : VK_FALSE;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = d.formats.samples;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = d.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = d.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = d.depth_compare;
    ds.stencilTestEnable = d.stencil_test ? VK_TRUE : VK_FALSE;
    ds.front = d.stencil_front, ds.back = d.stencil_back, ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = d.blend.enable ? VK_TRUE : VK_FALSE;
    attachment.srcColorBlendFactor = d.blend.src_color, attachment.dstColorBlendFactor = d.blend.dst_color;
    attachment.colorBlendOp = d.blend.color_op;
    attachment.srcAlphaBlendFactor = d.blend.src_alpha, attachment.dstAlphaBlendFactor = d.blend.dst_alpha;
    attachment.alphaBlendOp = d.blend.alpha_op, attachment.colorWriteMask = d.blend.write_mask;
    const bool has_color = d.formats.color != VK_FORMAT_UNDEFINED;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.logicOpEnable = d.blend.logic_enable && device_.caps().logic_op;
    blend.logicOp = d.blend.logic_op;
    blend.attachmentCount = has_color ? 1 : 0, blend.pAttachments = &attachment;
    if (d.formats.color_input && device_.caps().raster_order_attachment)
        blend.flags |= VK_PIPELINE_COLOR_BLEND_STATE_CREATE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_BIT_EXT;
    // (each part's dynamic state, as graphics() makes them dynamic)
    // (with dynamic_state, the rasterization, depth and stencil state too: the draws set them, set_dynamic_state)
    const VkDynamicState pre_raster[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_DEPTH_BIAS,
                                         VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
                                         VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE};
    const VkDynamicState fragment[] = {VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                                       VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                                       VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                                       VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP};
    const VkDynamicState output[] = {VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    const VkFormat color_format = d.formats.color;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = has_color ? 1 : 0, rendering.pColorAttachmentFormats = &color_format;
    rendering.depthAttachmentFormat = d.formats.depth;
    if (aspect_of(d.formats.depth) & VK_IMAGE_ASPECT_STENCIL_BIT) rendering.stencilAttachmentFormat = d.formats.depth;
    VkGraphicsPipelineLibraryCreateInfoEXT lib{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    rendering.pNext = &lib;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    switch (part) {
        case kVertexInput:
            lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT;
            info.pVertexInputState = &vertex, info.pInputAssemblyState = &assembly;
            break;
        case kPreRasterization:
            lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT;
            info.stageCount = 1, info.pStages = &stage, info.layout = d.layout;
            info.pViewportState = &viewport, info.pRasterizationState = &raster;
            dynamic.dynamicStateCount = dynamic_state_count(pre_raster, dynamic_states), dynamic.pDynamicStates = pre_raster;
            info.pDynamicState = &dynamic;
            break;
        case kFragmentShader:
            lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT;
            info.stageCount = 1, info.pStages = &stage, info.layout = d.layout;
            info.pDepthStencilState = &ds, info.pMultisampleState = &ms;
            dynamic.dynamicStateCount = dynamic_state_count(fragment, dynamic_states), dynamic.pDynamicStates = fragment;
            info.pDynamicState = &dynamic;
            break;
        default:
            lib.flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT;
            info.pColorBlendState = &blend, info.pMultisampleState = &ms;
            dynamic.dynamicStateCount = static_cast<u32>(std::size(output)), dynamic.pDynamicStates = output;
            info.pDynamicState = &dynamic;
            break;
    }
    VkPipeline p = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateGraphicsPipelines(device_.device(), cache_, 1, &info, nullptr, &p); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateGraphicsPipelines (a library)", r));
    nx::scoped_lock<nx::mutex> held(lock_);
    auto& list = libraries_[part][hash];
    for (const auto& [desc, other] : list)
        if (desc.same(d)) {
            vkDestroyPipeline(device_.device(), p, nullptr);
            return other;
        }
    list.push_back({d, p});
    return p;
}

Result<void> Pipelines::prepare(const GraphicsPipelineDesc& d) {
    // (a pass reading its colour as an input attachment uses a render pass: made whole, as graphics() makes it)
    if (!device_.caps().pipeline_library || passes_.uses_render_pass(d.formats)) return {};
    if (d.vertex)
        if (auto l = library(kPreRasterization, d); !l) return nx::err(l.error());
    if (d.fragment)
        if (auto l = library(kFragmentShader, d); !l) return nx::err(l.error());
    return {};
}

Result<VkPipeline> Pipelines::linked(const GraphicsPipelineDesc& d) {
    if (!device_.caps().pipeline_library || passes_.uses_render_pass(d.formats)) return graphics(d);
    if (const VkPipeline p = find(d)) return p;
    VkPipeline libs[4];
    for (u32 part = 0; part < 4; ++part) {
        auto l = library(part, d);
        if (!l) return nx::err(l.error());
        libs[part] = *l;
    }
    VkPipelineLibraryCreateInfoKHR from{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    from.libraryCount = 4, from.pLibraries = libs;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &from;
    info.layout = d.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateGraphicsPipelines(device_.device(), cache_, 1, &info, nullptr, &p); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateGraphicsPipelines (linked)", r));
    nx::scoped_lock<nx::mutex> held(lock_);
    if (const Entry* e = find_locked(d)) {
        vkDestroyPipeline(device_.device(), p, nullptr);
        return e->pipeline;
    }
    pipelines_[d.hash()].push_back({d, p, true});
    count_.fetch_add(1, std::memory_order_relaxed);
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
