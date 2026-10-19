#pragma once

// Graphics pipelines by description, made once and kept, with a VkPipelineCache that can be saved and loaded
// (the pre-warm of a later run). A description is hashed and compared as bytes: it has no padding (checked at
// compile time), every field is a fixed-size number or handle. Viewport, scissor, stencil reference and masks,
// depth bias and blend constants are dynamic state.

#include <span>

#include "psvrecore/vulkan/rendering.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/platform/filesystem.h"

namespace psvrecore::vulkan {

struct VertexBinding {
    u32 stride;
    u32 per_instance;  // (0: per vertex)
};

struct VertexAttribute {
    u32 location;
    u32 binding;
    VkFormat format;
    u32 offset;
};

struct BlendState {
    u32 enable;
    VkBlendFactor src_color, dst_color, src_alpha, dst_alpha;
    VkBlendOp color_op, alpha_op;
    VkColorComponentFlags write_mask;
    u32 logic_enable;
    VkLogicOp logic_op;
};

struct GraphicsPipelineDesc {
    static constexpr u32 kMaxBindings = 8, kMaxAttributes = 16, kMaxSpecialization = 8;

    VkShaderModule vertex;
    VkShaderModule fragment;
    VkPipelineLayout layout;
    VertexBinding bindings[kMaxBindings];
    VertexAttribute attributes[kMaxAttributes];
    u32 binding_count;
    u32 attribute_count;
    VkPrimitiveTopology topology;
    VkPolygonMode polygon;
    VkCullModeFlags cull;
    VkFrontFace front;
    u32 depth_clamp;
    u32 depth_bias;  // (enabled; the values are dynamic)
    u32 depth_test;
    u32 depth_write;
    VkCompareOp depth_compare;
    u32 stencil_test;
    VkStencilOpState stencil_front, stencil_back;  // (reference, compare and write masks: dynamic)
    BlendState blend;
    PassFormats formats;
    // Specialization constants 0 to count - 1, for both stages.
    u32 specialization[kMaxSpecialization];
    u32 specialization_count;
    u32 reserved;  // (zero: no padding at the end)

    // Every byte zero, then: triangles, filled, no culling, counter-clockwise front faces, no depth or stencil test,
    // no blending, all colour components written.
    static GraphicsPipelineDesc defaults();
    u64 hash() const;
    bool same(const GraphicsPipelineDesc& other) const;
};

class Pipelines {
public:
    Pipelines(Device& device, RenderPasses& passes);
    ~Pipelines();
    Pipelines(const Pipelines&) = delete;
    Pipelines& operator=(const Pipelines&) = delete;

    Result<VkPipeline> graphics(const GraphicsPipelineDesc& desc);
    usize count() const { return count_; }
    VkPipelineCache cache() const { return cache_; }  // (for the compute pipelines too)

    // The driver's pipeline cache, saved for the next run (a file another driver wrote is ignored).
    Result<void> save_cache(nx::fs::path_view path) const;
    Result<void> load_cache(nx::fs::path_view path);

private:
    Device& device_;
    RenderPasses& passes_;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    nx::hash_map<u64, nx::vector<std::pair<GraphicsPipelineDesc, VkPipeline>>> pipelines_;
    usize count_ = 0;
};

// A shader module from SPIR-V words.
Result<VkShaderModule> create_shader(Device& device, std::span<const u32> spirv);

}  // namespace psvrecore::vulkan
