#pragma once

// Graphics pipelines by description, made once and kept, with a VkPipelineCache that can be saved and loaded
// (the pre-warm of a later run). A description is hashed and compared as bytes: it has no padding (checked at
// compile time), every field is a fixed-size number or handle. Viewport, scissor, stencil reference and masks,
// depth bias and blend constants are dynamic state.

#include <atomic>
#include <span>

#include "psvrecore/vulkan/rendering.hpp"
#include "core/foundation/containers/hash_map.h"
#include "core/foundation/platform/filesystem.h"
#include "core/foundation/threading/sync.h"

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

    // Made once for each description, kept; may be called from several threads at once (the driver compiles
    // outside the lock: a phone's takes up to a tenth of a second for a pipeline whose shaders are new). It takes the
    // place of one linked() made for the description before.
    Result<VkPipeline> graphics(const GraphicsPipelineDesc& desc);
    // With VK_EXT_graphics_pipeline_library: the pipeline linked from libraries of its parts (each made once: its
    // vertex input, its vertex shader with the rasterization state, its fragment shader with the depth and stencil
    // state, its blending and formats), without link-time optimization; else graphics(). A phone's driver compiles a
    // shader for its library (20-70 ms), then once for every library of it (0.1 ms), and links in a millisecond or
    // two, where a whole pipeline's shaders are compiled together for each pair (25-630 ms).
    Result<VkPipeline> linked(const GraphicsPipelineDesc& desc);
    // The libraries of its shaders made, without a pipeline (the vertex shader's if it has one, the fragment shader's
    // if it has one): the driver compiles them into its cache. False when it makes none (no pipeline libraries, or a
    // render pass).
    Result<bool> prepare(const GraphicsPipelineDesc& desc);
    // Made before (VK_NULL_HANDLE: not yet); `linked`, whether it is one linked() made.
    VkPipeline find(const GraphicsPipelineDesc& desc, bool* linked = nullptr) const;
    usize count() const { return count_.load(std::memory_order_relaxed); }
    VkPipelineCache cache() const { return cache_; }  // (for the compute pipelines too)

    // The driver's pipeline cache, saved for the next run (a file another driver wrote is ignored).
    Result<void> save_cache(nx::fs::path_view path) const;
    Result<void> load_cache(nx::fs::path_view path);

private:
    struct Entry {
        GraphicsPipelineDesc desc;
        VkPipeline pipeline;
        bool linked;
    };
    const Entry* find_locked(const GraphicsPipelineDesc& desc) const;
    Result<VkPipeline> library(u32 part, const GraphicsPipelineDesc& desc);

    Device& device_;
    RenderPasses& passes_;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    mutable nx::mutex lock_;  // (the pipelines made, and the render passes they are made for)
    nx::hash_map<u64, nx::vector<Entry>> pipelines_;
    // (the libraries, by part: their descriptions hold only what the part reads; and linked pipelines whose place
    // graphics() took, destroyed at the end: scenes recorded with them may still be in flight)
    nx::hash_map<u64, nx::vector<std::pair<GraphicsPipelineDesc, VkPipeline>>> libraries_[4];
    nx::vector<VkPipeline> replaced_;
    std::atomic<usize> count_{0};
};

// A draw's rasterization, depth and stencil state, for a pipeline linked with them dynamic (Capabilities::
// dynamic_state): set for each draw (whole pipelines have them dynamic too, then).
void set_dynamic_state(VkCommandBuffer cmd, const GraphicsPipelineDesc& desc);

// The barrier a draw that reads the colour attachment needs after what the draws before it wrote (the pass's self-
// dependency: by region, the colour writes before the input attachment reads).
void color_read_barrier(VkCommandBuffer cmd);

// A draw of triangle lists (`count` vertices or indices) from the first, `instances` times, whose fragment shader
// reads the colour attachment (programmable blending). The guest GPU runs the primitives of a draw in order, each
// reading what the ones before it wrote; without rasterization-order attachment access (Capabilities::
// raster_order_attachment) the device does not, and primitives that overlap race, each reading the colour before the
// other's write lands (in blocks the size of the device's raster tiles). So there each triangle is its own draw with
// a barrier before it, the instances one after the other. With that access, one draw.
void draw_color_reading(VkCommandBuffer cmd, bool order_attachment, bool indexed, u32 count, u32 instances);

// A shader module from SPIR-V words.
Result<VkShaderModule> create_shader(Device& device, std::span<const u32> spirv);

}  // namespace psvrecore::vulkan
