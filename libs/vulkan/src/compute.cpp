// One-off compute dispatches (the shader translator's tests): every object is created for the dispatch and
// destroyed after it.

#include "core/foundation/core/scope_guard.h"
#include "core/foundation/strings/format.h"
#include "psvrecore/vulkan/device.hpp"

namespace psvrecore::vulkan {

Result<void> run_compute(Device& device, std::span<const u32> spirv, std::span<const Buffer* const> buffers,
                         u32 groups) {
    const VkDevice dev = device.device();
    auto check = [](const char* what, VkResult r) -> Result<void> {
        if (r != VK_SUCCESS) return failure(nx::format("{}: {}", what, result_name(r)));
        return {};
    };

    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkCommandPool commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    auto cleanup = nx::scope_guard([&] {
        if (fence) vkDestroyFence(dev, fence, nullptr);
        if (commands) vkDestroyCommandPool(dev, commands, nullptr);
        if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
        if (pipeline) vkDestroyPipeline(dev, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(dev, layout, nullptr);
        if (set_layout) vkDestroyDescriptorSetLayout(dev, set_layout, nullptr);
        if (module) vkDestroyShaderModule(dev, module, nullptr);
    });

    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = spirv.size() * 4;
    mi.pCode = spirv.data();
    if (auto r = check("vkCreateShaderModule", vkCreateShaderModule(dev, &mi, nullptr, &module)); !r) return r;

    nx::vector<VkDescriptorSetLayoutBinding> bindings(buffers.size());
    for (u32 i = 0; i < bindings.size(); ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    si.bindingCount = static_cast<u32>(bindings.size());
    si.pBindings = bindings.data();
    if (auto r = check("vkCreateDescriptorSetLayout", vkCreateDescriptorSetLayout(dev, &si, nullptr, &set_layout)); !r)
        return r;
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &set_layout;
    if (auto r = check("vkCreatePipelineLayout", vkCreatePipelineLayout(dev, &li, nullptr, &layout)); !r) return r;
    VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = module;
    pi.stage.pName = "main";
    pi.layout = layout;
    if (auto r = check("vkCreateComputePipelines",
                       vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &pipeline));
        !r)
        return r;

    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, static_cast<u32>(nx::max<usize>(buffers.size(), 1))};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &size;
    if (auto r = check("vkCreateDescriptorPool", vkCreateDescriptorPool(dev, &dpi, nullptr, &pool)); !r) return r;
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &set_layout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    if (auto r = check("vkAllocateDescriptorSets", vkAllocateDescriptorSets(dev, &dai, &set)); !r) return r;
    nx::vector<VkDescriptorBufferInfo> infos(buffers.size());
    nx::vector<VkWriteDescriptorSet> writes(buffers.size());
    for (u32 i = 0; i < buffers.size(); ++i) {
        infos[i] = VkDescriptorBufferInfo{buffers[i]->buffer, 0, VK_WHOLE_SIZE};
        writes[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(dev, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);

    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    ci.queueFamilyIndex = device.queue_family();
    if (auto r = check("vkCreateCommandPool", vkCreateCommandPool(dev, &ci, nullptr, &commands)); !r) return r;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = commands;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (auto r = check("vkAllocateCommandBuffers", vkAllocateCommandBuffers(dev, &cai, &cmd)); !r) return r;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdDispatch(cmd, groups, 1, 1);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &barrier, 0,
                         nullptr, 0, nullptr);
    if (auto r = check("vkEndCommandBuffer", vkEndCommandBuffer(cmd)); !r) return r;

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (auto r = check("vkCreateFence", vkCreateFence(dev, &fi, nullptr, &fence)); !r) return r;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    if (auto r = check("vkQueueSubmit", vkQueueSubmit(device.queue(), 1, &submit, fence)); !r) return r;
    if (auto r = check("vkWaitForFences", vkWaitForFences(dev, 1, &fence, VK_TRUE, 30'000'000'000ull)); !r) return r;
    return {};
}

}  // namespace psvrecore::vulkan
