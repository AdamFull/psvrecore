#include "psvrecore/gxm/half_check.hpp"

#include <cstdlib>
#include <cstring>

#include "core/foundation/platform/os.h"
#include "core/foundation/strings/format.h"
#include "psvrecore/log.hpp"
#include "psvrecore/vulkan/resources.hpp"

namespace psvrecore::gxm {

HalfCheck check_half_conversion(vulkan::Device& device) {
    HalfCheck c;
    if (const char* v = std::getenv("PSVRECORE_HALF"); v && std::strcmp(v, "integer") == 0) return c;
    const u64 start = nx::os::ticks_ns();
    const VkDevice dev = device.device();
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    vulkan::Buffer counts{};
    bool ok = false;
    do {
        const VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT,
                                                   nullptr};
        VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        si.bindingCount = 1;
        si.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(dev, &si, nullptr, &set_layout) != VK_SUCCESS) break;
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 4};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &set_layout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(dev, &li, nullptr, &layout) != VK_SUCCESS) break;
        const nx::vector<u32> code = half_check_shader();
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = code.size() * 4;
        mi.pCode = code.data();
        if (vkCreateShaderModule(dev, &mi, nullptr, &module) != VK_SUCCESS) break;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = layout;
        if (vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline) != VK_SUCCESS) break;
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &size;
        if (vkCreateDescriptorPool(dev, &pi, nullptr, &pool) != VK_SUCCESS) break;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        if (vkAllocateDescriptorSets(dev, &ai, &set) != VK_SUCCESS) break;
        auto buffer = vulkan::create_host_buffer(device, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (!buffer) break;
        counts = *buffer;
        const u32 initial[4] = {0, 0, 0, 0};
        std::memcpy(counts.mapped, initial, sizeof initial);
        vulkan::flush(device, counts);
        const VkDescriptorBufferInfo bi{counts.buffer, 0, 16};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        // (2^26 values a dispatch, 4096 x 256 workgroups of 64; a few dispatches a submission, each waited for)
        constexpr u64 kPerDispatch = 1ull << 26;
        constexpr u32 kPerSubmit = 4;
        ok = true;
        for (u64 first = 0; first < kHalfCheckValues && ok;) {
            vulkan::OneShot shot(device);
            auto cmd = shot.begin();
            if (!cmd) {
                ok = false;
                break;
            }
            vkCmdBindPipeline(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            for (u32 k = 0; k < kPerSubmit && first < kHalfCheckValues; ++k, first += kPerDispatch) {
                const u32 at = static_cast<u32>(first);
                vkCmdPushConstants(*cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &at);
                vkCmdDispatch(*cmd, 4096, 256, 1);
            }
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(*cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier,
                                 0, nullptr, 0, nullptr);
            ok = static_cast<bool>(shot.submit_and_wait());
        }
    } while (false);
    if (ok) {
        vulkan::invalidate(device, counts);
        u32 got[4];
        std::memcpy(got, counts.mapped, sizeof got);
        c.ran = true;
        c.differ = got[0] != 0;
        c.nans_differ = got[1] != 0;
        c.example = got[2];
        c.conversion = c.differ        ? HalfConversion::Integer
                       : c.nans_differ ? HalfConversion::DeviceNaN
                                       : HalfConversion::Device;
    }
    if (counts.buffer) vulkan::destroy_buffer(device, counts);
    if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
    if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
    if (module) vkDestroyShaderModule(dev, module, nullptr);
    if (layout) vkDestroyPipelineLayout(dev, layout, nullptr);
    if (set_layout) vkDestroyDescriptorSetLayout(dev, set_layout, nullptr);
    c.ms = static_cast<f64>(nx::os::ticks_ns() - start) / 1e6;
    if (!c.ran) {
        nx::logw(logs::gpu, "half precision: the device's conversion could not be checked; integer arithmetic");
        return c;
    }
    nx::logi(logs::gpu, "half precision: {} (the device's conversion of {} values checked in {:.0f} ms: {}; NaNs {})",
             half_conversion_name(c.conversion), kHalfCheckValues, c.ms,
             c.differ ? nx::format("some differ from the interpreter's, {:08x} one", c.example) : nx::string("the same"),
             c.nans_differ ? "differ" : "the same");
    return c;
}

}  // namespace psvrecore::gxm
