#include "gpu.hpp"

#include <bit>
#include <cstring>

#include "psvrecore/gxm/translate.hpp"
#include "psvrecore/usse/interp.hpp"
#include "psvrecore/vulkan/pipelines.hpp"
#include "core/foundation/strings/format.h"

namespace psvrender {

namespace {

namespace vk = vulkan;
using gxm::reference::Draw;
using gxm::reference::Texture;

struct Gpu {
    nx::unique_ptr<vk::Device> device;
    nx::unique_ptr<vk::RenderPasses> passes;
    nx::unique_ptr<vk::Pipelines> pipelines;
    nx::unique_ptr<vk::Samplers> samplers;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    ~Gpu() {
        if (!device) return;
        vkDestroyPipelineLayout(device->device(), layout, nullptr);
        vkDestroyDescriptorSetLayout(device->device(), set_layout, nullptr);
        samplers.reset();
        pipelines.reset();
        passes.reset();
        device.reset();
    }
};

// One device for the tool's run (the bindings: the HLE renderer's, without the colour input attachment).
Result<Gpu*> gpu(const GpuOptions& options) {
    static Gpu g;
    if (g.device) return &g;
    vk::DeviceOptions o;
    o.gpu = options.gpu;
    o.validation = options.validation;
    auto created = vk::Device::create(o);
    if (!created) return nx::err(created.error());
    g.device = std::move(*created);
    vk::Device& d = *g.device;
    g.passes = nx::make_unique<vk::RenderPasses>(d);
    g.pipelines = nx::make_unique<vk::Pipelines>(d, *g.passes);
    g.samplers = nx::make_unique<vk::Samplers>(d);
    VkDescriptorSetLayoutBinding bindings[gxm::kDrawBindingColor]{};
    for (u32 i = 0; i < gxm::kDrawBindingColor; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = i < gxm::kDrawBindingFragmentTextures ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                                            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = gxm::kDrawBindingColor;
    si.pBindings = bindings;
    vkCreateDescriptorSetLayout(d.device(), &si, nullptr, &g.set_layout);
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(gxm::DrawConstants)};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &g.set_layout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &push;
    vkCreatePipelineLayout(d.device(), &li, nullptr, &g.layout);
    return &g;
}

// The memory buffer's words (after its two-word header) as guest memory, for the secondary programs.
class MemoryView : public usse::ShaderMemory {
public:
    u32 base = 0;
    nx::vector<u32>* words = nullptr;
    bool read(u32 address, void* out, u32 size) override {
        if (address < base || u64{address} + size > u64{base} + words->size() * 4) return false;
        std::memcpy(out, reinterpret_cast<const u8*>(words->data()) + (address - base), size);
        return true;
    }
    bool write(u32 address, const void* in, u32 size) override {
        if (address < base || u64{address} + size > u64{base} + words->size() * 4) return false;
        std::memcpy(reinterpret_cast<u8*>(words->data()) + (address - base), in, size);
        return true;
    }
};

constexpr u32 kMemoryBase = 0x1000;

// A stage's secondary attributes: its buffers (and its default uniform buffer's copy) placed in memory, the
// driver's setup, its secondary program.
Result<void> secondary(const gxm::reference::Program& p, const nx::vector<u32>& uniforms,
                       const nx::vector<nx::vector<u32>>& buffers, nx::vector<u32>& mem, std::span<u32, 256> out) {
    u32 addresses[15] = {};
    auto place = [&](const nx::vector<u32>& words) {
        const u32 at = kMemoryBase + static_cast<u32>(mem.size()) * 4;
        mem.insert(mem.end(), words.begin(), words.end());
        mem.resize((mem.size() + 3) & ~usize{3}, 0);
        return at;
    };
    for (usize b = 0; b < buffers.size() && b < 14; ++b) addresses[b] = place(buffers[b]);
    addresses[usse::kDefaultUniformContainer] = place(uniforms);
    MemoryView view;
    view.base = kMemoryBase;
    view.words = &mem;
    usse::Machine m;
    m.memory = &view;
    nx::vector<usse::SamplerState> samplers(16);
    for (u32 k = 0; k < 16; ++k) samplers[k].words[0] = k;
    if (auto r = usse::load_secondary(*p.gxp, uniforms, addresses, m, samplers); !r) return r;
    if (!p.secondary.empty())
        if (auto r = usse::run(p.secondary, usse::ProgramKind::Secondary, m); !r) return r;
    std::memcpy(out.data(), m.secondary, 256 * 4);
    return {};
}

}  // namespace

Result<void> render_on_gpu(const GpuOptions& options, std::span<const Draw> draws, gxm::reference::Image& target) {
    auto g = gpu(options);
    if (!g) return nx::err(g.error());
    Gpu& G = **g;
    vk::Device& d = *G.device;
    const u32 W = target.width, H = target.height;

    nx::vector<vk::Image> images;  // (released at the end)
    nx::vector<vk::Buffer> buffers;
    nx::vector<VkShaderModule> modules;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    auto cleanup = [&] {
        if (pool) vkDestroyDescriptorPool(d.device(), pool, nullptr);
        for (vk::Image& im : images) {
            G.passes->forget(im.view);
            vk::destroy_image(d, im);
        }
        for (vk::Buffer& b : buffers) vk::destroy_buffer(d, b);
        for (VkShaderModule m : modules) vkDestroyShaderModule(d.device(), m, nullptr);
    };
    auto image = [&](const vk::ImageDesc& desc) -> Result<vk::Image*> {
        auto im = vk::create_image(d, desc);
        if (!im) return nx::err(im.error());
        images.push_back(*im);
        return &images.back();
    };
    auto buffer = [&](const void* data, usize bytes, VkBufferUsageFlags usage) -> Result<vk::Buffer*> {
        auto b = vk::create_host_buffer(d, nx::max<usize>(bytes, 16), usage);
        if (!b) return nx::err(b.error());
        if (data && bytes) std::memcpy(b->mapped, data, bytes);
        vk::flush(d, *b);
        buffers.push_back(*b);
        return &buffers.back();
    };
    images.reserve(4 + draws.size() * 16);
    buffers.reserve(4 + draws.size() * 8);

    Result<void> result = [&]() -> Result<void> {
        // The target, as it starts; depth cleared to 1.
        vk::ImageDesc cd;
        cd.width = W, cd.height = H;
        cd.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        auto color = image(cd);
        if (!color) return nx::err(color.error());
        if (auto r = vk::upload_image(d, **color, std::span(reinterpret_cast<const u8*>(target.rgba.data()), target.rgba.size() * 4),
                                      0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            !r)
            return r;
        vk::ImageDesc dd;
        dd.format = d.caps().depth_stencil_format;
        dd.width = W, dd.height = H;
        dd.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        auto depth = image(dd);
        if (!depth) return nx::err(depth.error());
        vk::ImageDesc wd;
        auto white = image(wd);
        if (!white) return nx::err(white.error());
        const u8 texel[4] = {255, 255, 255, 255};
        if (auto r = vk::upload_image(d, **white, texel); !r) return r;
        vk::SamplerDesc nearest;
        nearest.mag = nearest.min = VK_FILTER_NEAREST;
        vk::SamplerDesc linear;
        auto nearest_sampler = G.samplers->get(nearest);
        auto linear_sampler = G.samplers->get(linear);
        if (!nearest_sampler || !linear_sampler) return failure("no samplers");

        // Secondary attributes and memory, for every draw.
        nx::vector<u32> sa(draws.size() * gxm::kDrawSecondaryWords, 0), mem;
        for (usize i = 0; i < draws.size(); ++i) {
            const Draw& dr = draws[i];
            u32* at = sa.data() + i * gxm::kDrawSecondaryWords;
            if (auto r = secondary(dr.vertex, dr.vertex_uniforms, dr.vertex_buffers, mem, std::span<u32, 256>(at, 256)); !r)
                return r;
            if (auto r = secondary(dr.fragment, dr.fragment_uniforms, dr.fragment_buffers, mem, std::span<u32, 256>(at + 256, 256));
                !r)
                return r;
        }
        nx::vector<u32> mem_words{kMemoryBase, static_cast<u32>(mem.size())};
        mem_words.insert(mem_words.end(), mem.begin(), mem.end());
        mem_words.push_back(0), mem_words.push_back(0);
        auto sa_buf = buffer(sa.data(), sa.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        auto mem_buf = buffer(mem_words.data(), mem_words.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!sa_buf || !mem_buf) return failure("buffers");

        const u32 sets = static_cast<u32>(nx::max<usize>(draws.size(), 1));
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * sets},
                                              {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 20 * sets}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = sets;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        vkCreateDescriptorPool(d.device(), &pi, nullptr, &pool);

        struct Prepared {
            VkPipeline pipeline = VK_NULL_HANDLE;
            VkDescriptorSet set = VK_NULL_HANDLE;
            nx::vector<vk::Buffer*> streams;
            vk::Buffer* indices = nullptr;
            gxm::DrawConstants constants;
            u32 count = 0;
        };
        nx::vector<Prepared> prepared(draws.size());
        vk::Pass formats_pass;
        formats_pass.color.image = *color;
        formats_pass.depth.image = *depth;
        for (usize i = 0; i < draws.size(); ++i) {
            const Draw& dr = draws[i];
            Prepared& p = prepared[i];
            gxm::VertexShader vs;
            auto desc = vk::GraphicsPipelineDesc::defaults();
            for (usize a = 0; a < dr.attributes.size(); ++a) {
                const auto& attr = dr.attributes[a];
                gxm::VertexInput in;
                in.location = static_cast<u32>(a);
                in.reg = attr.reg;
                in.format = 9;  // SCE_GXM_ATTRIBUTE_FORMAT_F32
                in.components = static_cast<u8>(attr.components);
                for (const usse::Parameter& q : dr.vertex.gxp->parameters)
                    if (q.category == usse::ParameterCategory::Attribute && q.resource == attr.reg) in.raw = q.reg_format;
                vs.inputs.push_back(in);
                static constexpr VkFormat kF32[] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                                    VK_FORMAT_R32G32B32A32_SFLOAT};
                desc.bindings[a] = {attr.components * 4, 0};
                desc.attributes[a] = {static_cast<u32>(a), static_cast<u32>(a), kF32[nx::clamp<u32>(attr.components, 1, 4) - 1], 0};
                auto b = buffer(attr.values.data(), attr.values.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
                if (!b) return nx::err(b.error());
                p.streams.push_back(*b);
            }
            desc.binding_count = desc.attribute_count = static_cast<u32>(dr.attributes.size());
            auto vspirv = gxm::translate_vertex(*dr.vertex.gxp, dr.vertex.primary, vs);
            if (!vspirv) return failure("vertex program: " + vspirv.error().message);
            gxm::FragmentShader fs;
            auto fspirv = gxm::translate_fragment(*dr.fragment.gxp, dr.fragment.primary, fs);
            if (!fspirv) return failure("fragment program: " + fspirv.error().message);
            auto vm = vk::create_shader(d, *vspirv);
            if (!vm) return nx::err(vm.error());
            modules.push_back(*vm);
            auto fm = vk::create_shader(d, *fspirv);
            if (!fm) return nx::err(fm.error());
            modules.push_back(*fm);
            desc.vertex = *vm;
            desc.fragment = *fm;
            desc.layout = G.layout;
            if (dr.depth_test) {
                desc.depth_test = desc.depth_write = 1;
                desc.depth_compare = VK_COMPARE_OP_LESS_OR_EQUAL;
            }
            if (dr.blend) {
                desc.blend.enable = 1;
                desc.blend.color_op = desc.blend.alpha_op = VK_BLEND_OP_ADD;
                desc.blend.src_color = desc.blend.src_alpha = VK_BLEND_FACTOR_SRC_ALPHA;
                desc.blend.dst_color = desc.blend.dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            }
            desc.formats = formats_pass.formats();
            auto pipeline = G.pipelines->graphics(desc);
            if (!pipeline) return nx::err(pipeline.error());
            p.pipeline = *pipeline;
            auto ib = buffer(dr.indices.data(), dr.indices.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            if (!ib) return nx::err(ib.error());
            p.indices = *ib;
            p.count = static_cast<u32>(dr.indices.size());
            p.constants.draw = static_cast<u32>(i);
            p.constants.flags = gxm::kDrawViewport;
            p.constants.target[0] = static_cast<f32>(W), p.constants.target[1] = static_cast<f32>(H);
            const f32 vp[6] = {W * 0.5f, W * 0.5f, H * 0.5f, -(H * 0.5f), 0.5f, 0.5f};
            std::memcpy(p.constants.viewport, vp, sizeof vp);
            p.constants.w_clamp = 0.0f;

            // Descriptors: the buffers, the textures (white where a unit has none).
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = pool;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &G.set_layout;
            vkAllocateDescriptorSets(d.device(), &ai, &p.set);
            VkDescriptorBufferInfo bi[2] = {{(*sa_buf)->buffer, 0, VK_WHOLE_SIZE}, {(*mem_buf)->buffer, 0, VK_WHOLE_SIZE}};
            VkDescriptorImageInfo ii[gxm::kDrawBindingColor];
            for (u32 b = gxm::kDrawBindingFragmentTextures; b < gxm::kDrawBindingColor; ++b)
                ii[b] = {*nearest_sampler, (*white)->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            for (usize u = 0; u < dr.textures.size() && u < 16; ++u) {
                const Texture& t = dr.textures[u];
                vk::ImageDesc td;
                td.width = t.width, td.height = t.height;
                auto tex = image(td);
                if (!tex) return nx::err(tex.error());
                if (auto r = vk::upload_image(d, **tex, std::span(reinterpret_cast<const u8*>(t.texels.data()), t.texels.size() * 4));
                    !r)
                    return r;
                ii[gxm::kDrawBindingFragmentTextures + u] = {t.linear ? *linear_sampler : *nearest_sampler, (*tex)->view,
                                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            }
            VkWriteDescriptorSet w[gxm::kDrawBindingColor]{};
            for (u32 b = 0; b < gxm::kDrawBindingColor; ++b) {
                w[b] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[b].dstSet = p.set;
                w[b].dstBinding = b;
                w[b].descriptorCount = 1;
                if (b < gxm::kDrawBindingFragmentTextures) {
                    w[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    w[b].pBufferInfo = &bi[b];
                } else {
                    w[b].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    w[b].pImageInfo = &ii[b];
                }
            }
            vkUpdateDescriptorSets(d.device(), gxm::kDrawBindingColor, w, 0, nullptr);
        }

        // One pass: the target loaded, depth cleared.
        vk::OneShot shot(d);
        auto cmd = shot.begin();
        if (!cmd) return nx::err(cmd.error());
        vk::Pass pass;
        pass.color = {*color, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {}};
        pass.depth.image = *depth;
        pass.depth.clear_depth = 1.0f;
        if (auto begun = G.passes->begin(*cmd, pass); !begun) return begun;
        const VkViewport viewport{0, 0, static_cast<f32>(W), static_cast<f32>(H), 0, 1};
        const VkRect2D scissor{{0, 0}, {W, H}};
        vkCmdSetViewport(*cmd, 0, 1, &viewport);
        vkCmdSetScissor(*cmd, 0, 1, &scissor);
        const f32 blend[4] = {0, 0, 0, 0};
        for (const Prepared& p : prepared) {
            vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p.pipeline);
            vkCmdSetStencilReference(*cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
            vkCmdSetStencilCompareMask(*cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
            vkCmdSetStencilWriteMask(*cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
            vkCmdSetDepthBias(*cmd, 0, 0, 0);
            vkCmdSetBlendConstants(*cmd, blend);
            vkCmdBindDescriptorSets(*cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, G.layout, 0, 1, &p.set, 0, nullptr);
            vkCmdPushConstants(*cmd, G.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p.constants,
                               &p.constants);
            for (u32 s = 0; s < p.streams.size(); ++s) {
                const VkDeviceSize zero = 0;
                vkCmdBindVertexBuffers(*cmd, s, 1, &p.streams[s]->buffer, &zero);
            }
            vkCmdBindIndexBuffer(*cmd, p.indices->buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(*cmd, p.count, 1, 0, 0, 0);
        }
        G.passes->end(*cmd);
        if (auto done = shot.submit_and_wait(); !done) return done;
        auto pixels = vk::read_image(d, **color);
        if (!pixels) return nx::err(pixels.error());
        std::memcpy(target.rgba.data(), pixels->data(), nx::min<usize>(pixels->size(), target.rgba.size() * 4));
        if (d.validation_errors()) return failure(nx::format("{} Vulkan validation errors", d.validation_errors()));
        return {};
    }();
    cleanup();
    return result;
}

}  // namespace psvrender
