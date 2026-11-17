// psvrecore::vulkan on a real device: draws into images, read back and checked pixel by pixel, on both ways of
// rendering (dynamic rendering, render passes). Skipped without a Vulkan device (as on CI machines); the validation
// layer is on when installed, and its errors fail the tests. Shaders are built with gxm's SPIR-V builder.
// PSVRECORE_TEST_GPU=<index> picks the device (by default a discrete GPU).

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "nxtest.h"
#include "psvrecore/gxm/half_check.hpp"
#include "psvrecore/gxm/spirv.hpp"
#include "psvrecore/gxm/texture.hpp"
#include "psvrecore/vulkan/frames.hpp"
#include "psvrecore/vulkan/pipelines.hpp"
#include "spirv/unified1/spirv.h"
#include "core/foundation/strings/format.h"

using namespace psvrecore;
using namespace psvrecore::vulkan;
using gxm::spirv::Builder;
using gxm::spirv::Id;

namespace {

// ---- Shaders ---------------------------------------------------------------------------------------------------

// position (location 0, vec3) and a value (location 1, vec4) in; the value out (location 0).
nx::vector<u32> vertex_shader() {
    Builder b;
    b.capability(SpvCapabilityShader);
    const Id f = b.t_float(), v3 = b.t_vector(f, 3), v4 = b.t_vector(f, 4), tvoid = b.t_void();
    const Id in_pos = b.variable(b.t_pointer(SpvStorageClassInput, v3), SpvStorageClassInput);
    const Id in_val = b.variable(b.t_pointer(SpvStorageClassInput, v4), SpvStorageClassInput);
    const Id out_val = b.variable(b.t_pointer(SpvStorageClassOutput, v4), SpvStorageClassOutput);
    const Id out_pos = b.variable(b.t_pointer(SpvStorageClassOutput, v4), SpvStorageClassOutput);
    b.decorate(in_pos, SpvDecorationLocation, {0});
    b.decorate(in_val, SpvDecorationLocation, {1});
    b.decorate(out_val, SpvDecorationLocation, {0});
    b.decorate(out_pos, SpvDecorationBuiltIn, {SpvBuiltInPosition});
    const Id fn = b.begin_function(tvoid, b.t_function(tvoid));
    b.block(b.new_label());
    const Id p = b.op(SpvOpLoad, v3, {in_pos});
    Id xyz[3];
    for (u32 k = 0; k < 3; ++k) xyz[k] = b.op(SpvOpCompositeExtract, f, {p, k});
    b.op0(SpvOpStore, {out_pos, b.op(SpvOpCompositeConstruct, v4, {xyz[0], xyz[1], xyz[2], b.c_float(1.0f)})});
    b.op0(SpvOpStore, {out_val, b.op(SpvOpLoad, v4, {in_val})});
    b.op0(SpvOpReturn, {});
    b.end_function();
    const Id interface[] = {in_pos, in_val, out_val, out_pos};
    b.entry_point(SpvExecutionModelVertex, fn, "main", interface);
    return b.module();
}

enum class Fragment { Color, Texture, InvertInput };

// Color: the value; Texture: the texture of set 0, binding 0 at the value's x and y; InvertInput: 1 - the colour
// attachment (input attachment 0, set 0, binding 0).
nx::vector<u32> fragment_shader(Fragment kind) {
    Builder b;
    b.capability(SpvCapabilityShader);
    if (kind == Fragment::InvertInput) b.capability(SpvCapabilityInputAttachment);
    const Id f = b.t_float(), v2 = b.t_vector(f, 2), v4 = b.t_vector(f, 4), tvoid = b.t_void();
    const Id in_val = b.variable(b.t_pointer(SpvStorageClassInput, v4), SpvStorageClassInput);
    const Id out_col = b.variable(b.t_pointer(SpvStorageClassOutput, v4), SpvStorageClassOutput);
    b.decorate(in_val, SpvDecorationLocation, {0});
    b.decorate(out_col, SpvDecorationLocation, {0});
    Id image_type = 0, resource = 0;
    if (kind == Fragment::Texture) {
        image_type = b.t_sampled_image(b.t_image(f, SpvDim2D, 0, 0, 0, 1, SpvImageFormatUnknown));
    } else if (kind == Fragment::InvertInput) {
        image_type = b.t_image(f, SpvDimSubpassData, 0, 0, 0, 2, SpvImageFormatUnknown);
    }
    if (image_type) {
        resource = b.variable(b.t_pointer(SpvStorageClassUniformConstant, image_type), SpvStorageClassUniformConstant);
        b.decorate(resource, SpvDecorationDescriptorSet, {0});
        b.decorate(resource, SpvDecorationBinding, {0});
        if (kind == Fragment::InvertInput) b.decorate(resource, SpvDecorationInputAttachmentIndex, {0});
    }
    const Id fn = b.begin_function(tvoid, b.t_function(tvoid));
    b.block(b.new_label());
    const Id v = b.op(SpvOpLoad, v4, {in_val});
    Id out = v;
    if (kind == Fragment::Texture) {
        const Id uv = b.op(SpvOpVectorShuffle, v2, {v, v, 0, 1});
        out = b.op(SpvOpImageSampleImplicitLod, v4, {b.op(SpvOpLoad, image_type, {resource}), uv});
    } else if (kind == Fragment::InvertInput) {
        const Id i = b.t_int(), v2i = b.t_vector(i, 2);
        const Id texel = b.op(SpvOpImageRead, v4, {b.op(SpvOpLoad, image_type, {resource}),
                                                   b.c_composite(v2i, {b.c_int(0), b.c_int(0)})});
        const Id one = b.c_float(1.0f);
        out = b.op(SpvOpFSub, v4, {b.c_composite(v4, {one, one, one, one}), texel});
    }
    b.op0(SpvOpStore, {out_col, out});
    b.op0(SpvOpReturn, {});
    b.end_function();
    const Id interface[] = {in_val, out_col};
    b.entry_point(SpvExecutionModelFragment, fn, "main", interface);
    b.execution_mode(fn, SpvExecutionModeOriginUpperLeft);
    return b.module();
}

// ---- The device and drawing ------------------------------------------------------------------------------------

struct Vertex {
    f32 x, y, z;
    f32 v[4];
};

struct Gpu {
    nx::unique_ptr<Device> device;
    nx::unique_ptr<RenderPasses> passes;
    nx::unique_ptr<Pipelines> pipelines;
    VkShaderModule vs = VK_NULL_HANDLE;
    VkShaderModule fs[3] = {};
    VkDescriptorSetLayout set_layout[3] = {};  // (by Fragment)
    VkPipelineLayout layout[3] = {};
    nx::vector<Buffer> buffers;

    ~Gpu() {
        if (!device) return;
        device->wait_idle();
        VkDevice d = device->device();
        for (Buffer& b : buffers) destroy_buffer(*device, b);
        pipelines.reset();
        passes.reset();
        for (u32 i = 0; i < 3; ++i) {
            if (layout[i]) vkDestroyPipelineLayout(d, layout[i], nullptr);
            if (set_layout[i]) vkDestroyDescriptorSetLayout(d, set_layout[i], nullptr);
            if (fs[i]) vkDestroyShaderModule(d, fs[i], nullptr);
        }
        if (vs) vkDestroyShaderModule(d, vs, nullptr);
    }
};

// A device (with render passes forced when asked), or a skip without one.
nx::unique_ptr<Gpu> open(bool render_passes) {
    DeviceOptions o;
    o.validation = true;
    o.render_passes = render_passes;
    if (const char* gpu = std::getenv("PSVRECORE_TEST_GPU")) o.gpu = std::atoi(gpu);  // (the device's index)
    auto created = Device::create(o);
    if (!created) SKIP(nx::format("no Vulkan device: {}", created.error().message).c_str());
    auto g = nx::make_unique<Gpu>();
    g->device = std::move(*created);
    Device& d = *g->device;
    g->passes = nx::make_unique<RenderPasses>(d);
    g->pipelines = nx::make_unique<Pipelines>(d, *g->passes);
    g->vs = value_or_throw(create_shader(d, vertex_shader()));
    for (u32 i = 0; i < 3; ++i) {
        g->fs[i] = value_or_throw(create_shader(d, fragment_shader(static_cast<Fragment>(i))));
        VkDescriptorSetLayoutBinding binding{0, i == 1 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                                             1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        si.bindingCount = i ? 1 : 0;
        si.pBindings = &binding;
        vkCreateDescriptorSetLayout(d.device(), &si, nullptr, &g->set_layout[i]);
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &g->set_layout[i];
        vkCreatePipelineLayout(d.device(), &li, nullptr, &g->layout[i]);
    }
    return g;
}

GraphicsPipelineDesc desc_for(Gpu& g, Fragment kind, const Pass& pass) {
    auto d = GraphicsPipelineDesc::defaults();
    d.vertex = g.vs;
    d.fragment = g.fs[static_cast<u32>(kind)];
    d.layout = g.layout[static_cast<u32>(kind)];
    d.binding_count = 1;
    d.bindings[0] = {sizeof(Vertex), 0};
    d.attribute_count = 2;
    d.attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
    d.attributes[1] = {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 12};
    d.formats = pass.formats();
    return d;
}

// (the pipelines leave their rasterization, depth and stencil state dynamic where the device can: set from the
// description they were made from)
void bind(Gpu& g, VkCommandBuffer cmd, VkPipeline pipeline, const GraphicsPipelineDesc& desc) {
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (g.device->caps().dynamic_state) set_dynamic_state(cmd, desc);
}

void draw(Gpu& g, VkCommandBuffer cmd, VkPipeline pipeline, const GraphicsPipelineDesc& desc,
          std::span<const Vertex> vertices, u32 width, u32 height) {
    Buffer vb = value_or_throw(create_host_buffer(*g.device, vertices.size_bytes(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT));
    std::memcpy(vb.mapped, vertices.data(), vertices.size_bytes());
    flush(*g.device, vb);
    g.buffers.push_back(vb);
    const VkViewport viewport{0, 0, static_cast<f32>(width), static_cast<f32>(height), 0, 1};
    const VkRect2D scissor{{0, 0}, {width, height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    bind(g, cmd, pipeline, desc);
    const VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vb.buffer, &offset);
    vkCmdDraw(cmd, static_cast<u32>(vertices.size()), 1, 0, 0);
}

constexpr u32 kSize = 64;

ImageDesc target(VkFormat format = VK_FORMAT_R8G8B8A8_UNORM, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT) {
    ImageDesc d;
    d.format = format;
    d.width = d.height = kSize;
    d.samples = samples;
    d.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
              VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return d;
}

struct Rgba {
    u8 c[4];
};
Rgba pixel(const nx::vector<u8>& image, u32 x, u32 y) {
    const u8* p = image.data() + (y * kSize + x) * 4;
    return Rgba{{p[0], p[1], p[2], p[3]}};
}
bool near(Rgba got, u32 r, u32 g, u32 b, u32 a, u32 tolerance = 1) {
    const u32 want[4] = {r, g, b, a};
    for (u32 k = 0; k < 4; ++k)
        if ((got.c[k] > want[k] ? got.c[k] - want[k] : want[k] - got.c[k]) > tolerance) return false;
    return true;
}

// The triangle over the top-left half of the target: (x + y) < 64 in pixels.
nx::vector<Vertex> half(f32 z, f32 r, f32 g, f32 b, f32 a) {
    return {{-1, -1, z, {r, g, b, a}}, {1, -1, z, {r, g, b, a}}, {-1, 1, z, {r, g, b, a}}};
}
// A triangle over the whole target.
nx::vector<Vertex> full(f32 z, f32 r, f32 g, f32 b, f32 a) {
    return {{-1, -1, z, {r, g, b, a}}, {3, -1, z, {r, g, b, a}}, {-1, 3, z, {r, g, b, a}}};
}

void check_no_validation_errors(Gpu& g) { CHECK(g.device->validation_errors() == 0); }

}  // namespace

TEST_CASE("vulkan gpu: a triangle over a cleared target") {
    for (const bool render_passes : {false, true}) {
        auto g = open(render_passes);
        Image color = value_or_throw(create_image(*g->device, target()));
        Pass pass;
        pass.color = {&color, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{0.0f, 0.0f, 1.0f, 1.0f}}};
        const auto desc = desc_for(*g, Fragment::Color, pass);
        const VkPipeline p = value_or_throw(g->pipelines->graphics(desc));
        OneShot shot(*g->device);
        VkCommandBuffer cmd = value_or_throw(shot.begin());
        REQUIRE(g->passes->begin(cmd, pass));
        draw(*g, cmd, p, desc, half(0.5f, 1, 0, 0, 1), kSize, kSize);
        g->passes->end(cmd);
        REQUIRE(shot.submit_and_wait());
        const auto image = value_or_throw(read_image(*g->device, color));
        CHECK(near(pixel(image, 4, 4), 255, 0, 0, 255, 0));
        CHECK(near(pixel(image, 20, 40), 255, 0, 0, 255, 0));
        CHECK(near(pixel(image, 60, 60), 0, 0, 255, 255, 0));
        CHECK(near(pixel(image, 40, 30), 0, 0, 255, 255, 0));
        // (the same description gives the same pipeline)
        CHECK(value_or_throw(g->pipelines->graphics(desc_for(*g, Fragment::Color, pass))) == p);
        CHECK(g->pipelines->count() == 1);
        g->passes->forget(color.view);
        destroy_image(*g->device, color);
        check_no_validation_errors(*g);
    }
}

TEST_CASE("vulkan gpu: depth test and blending") {
    for (const bool render_passes : {false, true}) {
        auto g = open(render_passes);
        Device& d = *g->device;
        Image color = value_or_throw(create_image(d, target()));
        ImageDesc dd = target(d.caps().depth_stencil_format);
        dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        Image depth = value_or_throw(create_image(d, dd));
        Pass pass;
        pass.color = {&color, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{0.0f, 0.0f, 1.0f, 1.0f}}};
        pass.depth.image = &depth;
        auto near_desc = desc_for(*g, Fragment::Color, pass);
        near_desc.depth_test = near_desc.depth_write = 1;
        near_desc.depth_compare = VK_COMPARE_OP_LESS;
        auto far_desc = near_desc;
        far_desc.blend.enable = 1;
        far_desc.blend.src_color = VK_BLEND_FACTOR_SRC_ALPHA;
        far_desc.blend.dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        const VkPipeline pn = value_or_throw(g->pipelines->graphics(near_desc));
        const VkPipeline pf = value_or_throw(g->pipelines->graphics(far_desc));
        OneShot shot(d);
        VkCommandBuffer cmd = value_or_throw(shot.begin());
        REQUIRE(g->passes->begin(cmd, pass));
        draw(*g, cmd, pn, near_desc, half(0.25f, 1, 0, 0, 1), kSize, kSize);  // near: red
        draw(*g, cmd, pf, far_desc, full(0.75f, 0, 1, 0, 0.5f), kSize, kSize);  // far: green at half, blended where it passes
        g->passes->end(cmd);
        REQUIRE(shot.submit_and_wait());
        const auto image = value_or_throw(read_image(d, color));
        CHECK(near(pixel(image, 8, 8), 255, 0, 0, 255, 0));    // (behind the red triangle: not drawn)
        CHECK(near(pixel(image, 56, 56), 0, 128, 128, 128));  // blue blended with half green (alpha: the source's)
        CHECK(g->pipelines->count() == 2);
        g->passes->forget(color.view);
        g->passes->forget(depth.view);
        destroy_image(d, color);
        destroy_image(d, depth);
        check_no_validation_errors(*g);
    }
}

TEST_CASE("vulkan gpu: texture sampling") {
    for (const bool render_passes : {false, true}) {
        auto g = open(render_passes);
        Device& d = *g->device;
        ImageDesc td;
        td.width = td.height = 2;
        Image tex = value_or_throw(create_image(d, td));
        const u8 texels[16] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
        REQUIRE(upload_image(d, tex, texels));
        Samplers samplers(d);
        SamplerDesc sd;
        sd.mag = sd.min = VK_FILTER_NEAREST;
        sd.u = sd.v = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        const VkSampler sampler = value_or_throw(samplers.get(sd));
        CHECK(value_or_throw(samplers.get(sd)) == sampler);

        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        VkDescriptorPool pool;
        REQUIRE(vkCreateDescriptorPool(d.device(), &pi, nullptr, &pool) == VK_SUCCESS);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &g->set_layout[1];
        VkDescriptorSet set;
        REQUIRE(vkAllocateDescriptorSets(d.device(), &ai, &set) == VK_SUCCESS);
        VkDescriptorImageInfo ii{sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        vkUpdateDescriptorSets(d.device(), 1, &w, 0, nullptr);

        Image color = value_or_throw(create_image(d, target()));
        Pass pass;
        pass.color = {&color, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {}};
        const auto desc = desc_for(*g, Fragment::Texture, pass);
        const VkPipeline p = value_or_throw(g->pipelines->graphics(desc));
        // (the value is the texture coordinate: 0-1 across the target)
        const Vertex quad[] = {{-1, -1, 0, {0, 0, 0, 0}}, {3, -1, 0, {2, 0, 0, 0}}, {-1, 3, 0, {0, 2, 0, 0}}};
        OneShot shot(d);
        VkCommandBuffer cmd = value_or_throw(shot.begin());
        REQUIRE(g->passes->begin(cmd, pass));
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout[1], 0, 1, &set, 0, nullptr);
        draw(*g, cmd, p, desc, quad, kSize, kSize);
        g->passes->end(cmd);
        REQUIRE(shot.submit_and_wait());
        const auto image = value_or_throw(read_image(d, color));
        CHECK(near(pixel(image, 10, 10), 255, 0, 0, 255, 0));
        CHECK(near(pixel(image, 50, 10), 0, 255, 0, 255, 0));
        CHECK(near(pixel(image, 10, 50), 0, 0, 255, 255, 0));
        CHECK(near(pixel(image, 50, 50), 255, 255, 255, 255, 0));
        vkDestroyDescriptorPool(d.device(), pool, nullptr);
        g->passes->forget(color.view);
        destroy_image(d, color);
        destroy_image(d, tex);
        check_no_validation_errors(*g);
    }
}

TEST_CASE("vulkan gpu: multisampling with a resolve") {
    for (const bool render_passes : {false, true}) {
        auto g = open(render_passes);
        Device& d = *g->device;
        ImageDesc md = target(VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT);
        md.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
        Image ms = value_or_throw(create_image(d, md));
        Image color = value_or_throw(create_image(d, target()));
        Pass pass;
        pass.color = {&ms, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, {{0, 0, 0, 1}}, &color};
        const auto desc = desc_for(*g, Fragment::Color, pass);
        const VkPipeline p = value_or_throw(g->pipelines->graphics(desc));
        OneShot shot(d);
        VkCommandBuffer cmd = value_or_throw(shot.begin());
        REQUIRE(g->passes->begin(cmd, pass));
        draw(*g, cmd, p, desc, half(0, 1, 1, 1, 1), kSize, kSize);
        g->passes->end(cmd);
        REQUIRE(shot.submit_and_wait());
        const auto image = value_or_throw(read_image(d, color));
        CHECK(near(pixel(image, 8, 8), 255, 255, 255, 255, 0));
        CHECK(near(pixel(image, 56, 56), 0, 0, 0, 255, 0));
        // On the diagonal edge some pixels are partly covered: their resolved value is between the two.
        u32 between = 0;
        for (u32 x = 0; x < kSize; ++x) {
            const Rgba c = pixel(image, x, kSize - 1 - x);
            between += c.c[0] > 10 && c.c[0] < 245;
        }
        CHECK(between > 0);
        g->passes->forget(ms.view);
        g->passes->forget(color.view);
        destroy_image(d, ms);
        destroy_image(d, color);
        check_no_validation_errors(*g);
    }
}

TEST_CASE("vulkan gpu: reading the colour attachment (programmable blending)") {
    auto g = open(false);
    Device& d = *g->device;
    Image color = value_or_throw(create_image(d, target()));
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    VkDescriptorPool pool;
    REQUIRE(vkCreateDescriptorPool(d.device(), &pi, nullptr, &pool) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g->set_layout[2];
    VkDescriptorSet set;
    REQUIRE(vkAllocateDescriptorSets(d.device(), &ai, &set) == VK_SUCCESS);
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, color.view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(d.device(), 1, &w, 0, nullptr);

    Pass pass;
    pass.color = {&color, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {{0.25f, 0.5f, 0.75f, 1.0f}}};
    pass.color_input = true;
    const auto desc = desc_for(*g, Fragment::InvertInput, pass);
    const VkPipeline p = value_or_throw(g->pipelines->graphics(desc));
    OneShot shot(d);
    VkCommandBuffer cmd = value_or_throw(shot.begin());
    REQUIRE(g->passes->begin(cmd, pass));
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout[2], 0, 1, &set, 0, nullptr);
    draw(*g, cmd, p, desc, half(0, 0, 0, 0, 0), kSize, kSize);
    if (!d.caps().raster_order_attachment) {
        // (without rasterization-order access, a barrier between draws that read what the one before wrote)
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             VK_DEPENDENCY_BY_REGION_BIT, 1, &b, 0, nullptr, 0, nullptr);
    }
    draw(*g, cmd, p, desc, full(0, 0, 0, 0, 0), kSize, kSize);  // (inverts again where the first one drew)
    g->passes->end(cmd);
    REQUIRE(shot.submit_and_wait());
    const auto image = value_or_throw(read_image(d, color));
    CHECK(near(pixel(image, 8, 8), 64, 128, 191, 255));    // inverted twice
    CHECK(near(pixel(image, 56, 56), 191, 128, 64, 0));    // inverted once (alpha 1 - 1)
    vkDestroyDescriptorPool(d.device(), pool, nullptr);
    g->passes->forget(color.view);
    destroy_image(d, color);
    check_no_validation_errors(*g);
}

TEST_CASE("vulkan gpu: frames in flight, streamed data and deferred releases") {
    auto g = open(false);
    Device& d = *g->device;
    u32 released = 0;  // (before the frames: their destructor runs what is still deferred)
    Frames frames(d);
    Image color = value_or_throw(create_image(d, target()));
    Pass pass;
    pass.color = {&color, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE, {}};
    const auto desc = desc_for(*g, Fragment::Color, pass);
    const VkPipeline p = value_or_throw(g->pipelines->graphics(desc));
    for (u32 i = 0; i < 5; ++i) {
        VkCommandBuffer cmd = value_or_throw(frames.begin());
        const auto vertices = full(0, static_cast<f32>(i) / 4.0f, 0, 0, 1);
        const auto slice = value_or_throw(frames.stream(sizeof(Vertex) * vertices.size(), 16));
        std::memcpy(slice.data, vertices.data(), sizeof(Vertex) * vertices.size());
        frames.defer([&released] { ++released; });
        REQUIRE(g->passes->begin(cmd, pass));
        const VkViewport viewport{0, 0, kSize, kSize, 0, 1};
        const VkRect2D scissor{{0, 0}, {kSize, kSize}};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        bind(*g, cmd, p, desc);
        vkCmdBindVertexBuffers(cmd, 0, 1, &slice.buffer, &slice.offset);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        g->passes->end(cmd);
        REQUIRE(frames.submit());
    }
    frames.wait_all();
    CHECK(released == 5 - Frames::kFrames);  // (the last frames' releases wait for their next turn)
    const auto image = value_or_throw(read_image(d, color));
    CHECK(near(pixel(image, 32, 32), 255, 0, 0, 255, 0));  // the last frame's colour
    g->passes->forget(color.view);
    destroy_image(d, color);
    check_no_validation_errors(*g);
}

TEST_CASE("vulkan gpu: YUV420 textures converted by a compute shader as on the CPU") {
    auto g = open(false);
    Device& d = *g->device;
    const VkShaderModule module = value_or_throw(create_shader(d, gxm::yuv420_shader()));
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = 1;
    si.pBindings = &binding;
    VkDescriptorSetLayout set_layout;
    REQUIRE(vkCreateDescriptorSetLayout(d.device(), &si, nullptr, &set_layout) == VK_SUCCESS);
    const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(gxm::Yuv420Constants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &set_layout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    VkPipelineLayout layout;
    REQUIRE(vkCreatePipelineLayout(d.device(), &li, nullptr, &layout) == VK_SUCCESS);
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = module;
    ci.stage.pName = "main";
    ci.layout = layout;
    VkPipeline pipeline;
    REQUIRE(vkCreateComputePipelines(d.device(), VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) == VK_SUCCESS);
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pi.maxSets = 1;
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &size;
    VkDescriptorPool pool;
    REQUIRE(vkCreateDescriptorPool(d.device(), &pi, nullptr, &pool) == VK_SUCCESS);

    // (planes of noise: every byte value, in every channel's tables)
    nx::vector<u8> guest(2 * 1024 * 1024);
    u32 seed = 12345;
    for (u8& v : guest) seed = seed * 1103515245u + 12345u, v = static_cast<u8>(seed >> 16);
    auto read = [&](u32 address, void* out, u32 bytes) {
        if (address + u64{bytes} > guest.size()) return false;
        std::memcpy(out, guest.data() + address, bytes);
        return true;
    };
    struct Case {
        u32 format, width, height, swizzle, profile;
        bool padded;
    };
    const Case cases[] = {
        {0x90000000u, 960, 544, 0, 0, false}, {0x90000000u, 37, 21, 1, 1, false}, {0x91000000u, 64, 48, 2, 2, false},
        {0x91000000u, 35, 19, 3, 3, false},   {0x90000000u, 50, 30, 2, 1, true},  {0x91000000u, 50, 30, 1, 0, true},
    };
    for (const Case& c : cases) {
        gxm::set_yuv_profile((c.swizzle >> 1) & 1, c.profile);
        gxm::TextureInfo t;
        t.base_format = c.format, t.width = c.width, t.height = c.height, t.swizzle = c.swizzle;
        t.mipmapped = c.padded;
        t.address = 4096;
        const auto cpu = value_or_throw(gxm::read_texels(t, read));
        const auto planes = value_or_throw(gxm::read_yuv420(t, read));
        const usize bytes = gxm::yuv420_buffer_bytes(planes);
        Buffer buf = value_or_throw(create_host_buffer(d, bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
        const gxm::Yuv420Constants k = gxm::yuv420_buffer(planes, static_cast<u8*>(buf.mapped));
        flush(d, buf);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        VkDescriptorSet set;
        REQUIRE(vkAllocateDescriptorSets(d.device(), &ai, &set) == VK_SUCCESS);
        VkDescriptorBufferInfo bi{buf.buffer, 0, bytes};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(d.device(), 1, &w, 0, nullptr);
        OneShot shot(d);
        VkCommandBuffer cmd = value_or_throw(shot.begin());
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof k, &k);
        vkCmdDispatch(cmd, (c.width + 7) / 8, (c.height + 7) / 8, 1);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                             nullptr);
        REQUIRE(shot.submit_and_wait());
        invalidate(d, buf);
        REQUIRE(cpu.data.size() == usize{c.width} * c.height * 4);
        const u8* gpu = static_cast<const u8*>(buf.mapped) + usize{k.out} * 4;
        usize differ = 0, first = 0;
        for (usize i = 0; i < cpu.data.size(); ++i)
            if (gpu[i] != cpu.data[i] && differ++ == 0) first = i;
        if (differ)
            std::fprintf(stderr, "YUV case format %08x, %u x %u, swizzle %u, profile %u: %zu bytes differ, the first at %zu\n",
                         c.format, c.width, c.height, c.swizzle, c.profile, differ, first);
        CHECK(differ == 0);
        vkFreeDescriptorSets(d.device(), pool, 1, &set);
        destroy_buffer(d, buf);
    }
    gxm::set_yuv_profile(0, 0), gxm::set_yuv_profile(1, 1);
    vkDestroyDescriptorPool(d.device(), pool, nullptr);
    vkDestroyPipeline(d.device(), pipeline, nullptr);
    vkDestroyPipelineLayout(d.device(), layout, nullptr);
    vkDestroyDescriptorSetLayout(d.device(), set_layout, nullptr);
    vkDestroyShaderModule(d.device(), module, nullptr);
    check_no_validation_errors(*g);
}

TEST_CASE("vulkan gpu: the device's conversion to half precision checked against the interpreter's") {
    auto g = open(false);
    const gxm::HalfCheck c = gxm::check_half_conversion(*g->device);
    REQUIRE(c.ran);
    // (which way is used depends on the device; a device's conversion that rounds as the interpreter's is used)
    CHECK(c.conversion == (c.differ ? gxm::HalfConversion::Integer
                           : c.nans_differ ? gxm::HalfConversion::DeviceNaN : gxm::HalfConversion::Device));
    std::printf("half precision: %s\n", gxm::half_conversion_name(c.conversion));
    check_no_validation_errors(*g);
}
