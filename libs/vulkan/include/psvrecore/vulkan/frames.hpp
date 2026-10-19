#pragma once

// Frames in flight: each has a command buffer, a fence, a stream of host-visible memory for the data its draws use
// (vertices, indices, uniforms; reset when the frame comes round again) and the objects to destroy once the GPU is
// done with it.

#include "psvrecore/vulkan/resources.hpp"
#include "core/foundation/core/callable.h"

namespace psvrecore::vulkan {

class Frames {
public:
    static constexpr u32 kFrames = 2;
    static constexpr VkDeviceSize kStreamChunk = 8u << 20;

    explicit Frames(Device& device);
    ~Frames();
    Frames(const Frames&) = delete;
    Frames& operator=(const Frames&) = delete;

    // Waits until the next frame's previous use is done, releases what it deferred and starts its command buffer.
    Result<VkCommandBuffer> begin();
    // Submits the frame (waiting for `wait` at `wait_stage` and signalling `signal` when they are given).
    Result<void> submit(VkSemaphore wait = VK_NULL_HANDLE, VkPipelineStageFlags wait_stage = 0,
                        VkSemaphore signal = VK_NULL_HANDLE);
    // Waits for every frame.
    void wait_all();

    struct Slice {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        void* data = nullptr;
    };
    // `size` bytes of the frame's stream, aligned (usable as vertex, index, uniform and storage data).
    Result<Slice> stream(VkDeviceSize size, VkDeviceSize alignment = 16);
    // Runs `release` once the GPU has finished the current frame.
    void defer(nx::unique_function<void()> release);
    template <class F>
        requires(!std::is_same_v<std::decay_t<F>, nx::unique_function<void()>>)
    void defer(F&& release) {
        defer(nx::unique_function<void()>(std::forward<F>(release)));
    }

    VkCommandBuffer cmd() const { return frames_[current_].cmd; }
    u64 number() const { return number_; }  // frames begun so far

private:
    struct Chunk {
        Buffer buffer;
        VkDeviceSize used = 0;
    };
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        nx::vector<Chunk> chunks;
        nx::vector<nx::unique_function<void()>> deferred;
        bool submitted = false;
    };

    Device& device_;
    Frame frames_[kFrames];
    u32 current_ = 0;
    u64 number_ = 0;
    nx::string error_;
};

}  // namespace psvrecore::vulkan
