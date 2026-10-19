#include "psvrecore/vulkan/frames.hpp"

namespace psvrecore::vulkan {

Frames::Frames(Device& device) : device_(device) {
    for (Frame& f : frames_) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = device_.queue_family();
        VkResult r = vkCreateCommandPool(device_.device(), &pi, nullptr, &f.pool);
        if (r == VK_SUCCESS) {
            VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            ai.commandPool = f.pool;
            ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ai.commandBufferCount = 1;
            r = vkAllocateCommandBuffers(device_.device(), &ai, &f.cmd);
        }
        if (r == VK_SUCCESS) {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            r = vkCreateFence(device_.device(), &fi, nullptr, &f.fence);
        }
        if (r != VK_SUCCESS && error_.empty()) error_ = vk_error("frame setup", r).message;
    }
}

Frames::~Frames() {
    wait_all();
    for (Frame& f : frames_) {
        for (auto& release : f.deferred) release();
        for (Chunk& c : f.chunks) destroy_buffer(device_, c.buffer);
        if (f.fence) vkDestroyFence(device_.device(), f.fence, nullptr);
        if (f.pool) vkDestroyCommandPool(device_.device(), f.pool, nullptr);
    }
}

void Frames::wait_all() {
    for (Frame& f : frames_) {
        if (!f.submitted) continue;
        vkWaitForFences(device_.device(), 1, &f.fence, VK_TRUE, UINT64_MAX);
        vkResetFences(device_.device(), 1, &f.fence);
        f.submitted = false;
    }
}

Result<VkCommandBuffer> Frames::begin() {
    if (!error_.empty()) return failure(error_);
    current_ = static_cast<u32>(number_ % kFrames);
    ++number_;
    Frame& f = frames_[current_];
    if (f.submitted) {
        if (const VkResult r = vkWaitForFences(device_.device(), 1, &f.fence, VK_TRUE, UINT64_MAX); r != VK_SUCCESS)
            return nx::err(vk_error("vkWaitForFences", r));
        vkResetFences(device_.device(), 1, &f.fence);
        f.submitted = false;
    }
    for (auto& release : f.deferred) release();
    f.deferred.clear();
    for (Chunk& c : f.chunks) c.used = 0;
    vkResetCommandPool(device_.device(), f.pool, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (const VkResult r = vkBeginCommandBuffer(f.cmd, &bi); r != VK_SUCCESS)
        return nx::err(vk_error("vkBeginCommandBuffer", r));
    return f.cmd;
}

Result<void> Frames::submit(VkSemaphore wait, VkPipelineStageFlags wait_stage, VkSemaphore signal) {
    Frame& f = frames_[current_];
    for (Chunk& c : f.chunks)
        if (c.used) flush(device_, c.buffer);
    if (const VkResult r = vkEndCommandBuffer(f.cmd); r != VK_SUCCESS) return nx::err(vk_error("vkEndCommandBuffer", r));
    VkSubmitInfo info{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    info.commandBufferCount = 1;
    info.pCommandBuffers = &f.cmd;
    if (wait) {
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &wait;
        info.pWaitDstStageMask = &wait_stage;
    }
    if (signal) {
        info.signalSemaphoreCount = 1;
        info.pSignalSemaphores = &signal;
    }
    if (const VkResult r = device_.submit(info, f.fence); r != VK_SUCCESS) return nx::err(vk_error("vkQueueSubmit", r));
    f.submitted = true;
    return {};
}

Result<Frames::Slice> Frames::stream(VkDeviceSize size, VkDeviceSize alignment) {
    Frame& f = frames_[current_];
    for (Chunk& c : f.chunks) {
        const VkDeviceSize at = (c.used + alignment - 1) / alignment * alignment;
        if (at + size <= c.buffer.size) {
            c.used = at + size;
            return Slice{c.buffer.buffer, at, static_cast<u8*>(c.buffer.mapped) + at};
        }
    }
    auto b = create_host_buffer(device_, nx::max(size, kStreamChunk),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (!b) return nx::err(b.error());
    f.chunks.push_back(Chunk{*b, size});
    return Slice{b->buffer, 0, b->mapped};
}

void Frames::defer(nx::unique_function<void()> release) { frames_[current_].deferred.push_back(std::move(release)); }

}  // namespace psvrecore::vulkan
