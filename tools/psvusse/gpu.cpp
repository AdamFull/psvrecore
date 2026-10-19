#include "gpu.hpp"

#include <cstring>

#include "psvrecore/gxm/translate.hpp"
#include "psvrecore/vulkan/device.hpp"
#include "core/foundation/strings/format.h"

namespace psvusse {

namespace {

using gxm::ComputeLayout;

// One device for every program the tool runs.
Result<vulkan::Device*> device(const GpuOptions& options) {
    static nx::unique_ptr<vulkan::Device> d;
    if (!d) {
        vulkan::DeviceOptions o;
        o.gpu = options.gpu;
        o.validation = options.validation;
        auto created = vulkan::Device::create(o);
        if (!created) return nx::err(created.error());
        d = std::move(*created);
    }
    return d.get();
}

}  // namespace

Result<void> run_on_gpu(const GpuOptions& options, const usse::Gxp& gxp, std::span<const usse::Decoded> program,
                        usse::Machine& m, std::span<const GpuTexture> textures, u32 memory_base,
                        std::span<const u32> memory) {
    auto spirv = gxm::translate_compute(gxp, program);
    if (!spirv) return nx::err(spirv.error());
    auto dev = device(options);
    if (!dev) return nx::err(dev.error());
    vulkan::Device& d = **dev;

    nx::vector<u32> tex(ComputeLayout::kTextures * 4, 0);
    for (const GpuTexture& t : textures) {
        if (t.unit >= ComputeLayout::kTextures) continue;
        tex[t.unit * 4 + 0] = t.width;
        tex[t.unit * 4 + 1] = t.height;
        tex[t.unit * 4 + 2] = static_cast<u32>(tex.size());
        tex.insert(tex.end(), t.texels.begin(), t.texels.end());
    }
    nx::vector<u32> mem{memory_base, static_cast<u32>(memory.size())};
    mem.insert(mem.end(), memory.begin(), memory.end());
    mem.push_back(0);
    mem.push_back(0);

    const nx::vector<u32> in(m.primary, m.primary + ComputeLayout::kIn);
    const nx::vector<u32> sa(m.secondary, m.secondary + ComputeLayout::kSecondary);
    const std::span<const u32> contents[] = {in, sa, {}, tex, mem};
    const usize sizes[] = {in.size(), sa.size(), ComputeLayout::kOut, tex.size(), mem.size()};
    vulkan::Buffer buffers[5];
    Result<void> result;
    for (u32 i = 0; i < 5 && result; ++i) {
        auto b = vulkan::create_host_buffer(d, sizes[i] * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!b) {
            result = nx::err(b.error());
            break;
        }
        buffers[i] = *b;
        std::memset(buffers[i].mapped, 0, sizes[i] * 4);
        if (!contents[i].empty()) std::memcpy(buffers[i].mapped, contents[i].data(), contents[i].size() * 4);
        vulkan::flush(d, buffers[i]);
    }
    if (result) {
        const vulkan::Buffer* bound[] = {&buffers[0], &buffers[1], &buffers[2], &buffers[3], &buffers[4]};
        result = vulkan::run_compute(d, *spirv, bound, 1);
    }
    if (result) {
        vulkan::invalidate(d, buffers[2]);
        const u32* out = static_cast<const u32*>(buffers[2].mapped);
        std::memcpy(m.primary, out, ComputeLayout::kPrimary * 4);
        std::memcpy(m.output, out + ComputeLayout::kPrimary, ComputeLayout::kOutput * 4);
        const u32 flags = out[ComputeLayout::kPrimary + ComputeLayout::kOutput];
        m.killed = (flags & 1) != 0;
        if (flags & 2) result = failure("step limit reached");
    }
    for (vulkan::Buffer& b : buffers) vulkan::destroy_buffer(d, b);
    if (result && d.validation_errors())
        result = failure(nx::format("{} Vulkan validation errors", d.validation_errors()));
    return result;
}

}  // namespace psvusse
