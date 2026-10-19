#pragma once

// The Vulkan device: an instance (with the validation layer on request), the physical device chosen, a logical
// device with one queue that does graphics and compute, and a VMA allocator. Vulkan 1.1 is the minimum: what the
// GXM layer needs beyond it is checked as optional features.

#include <span>

#include "psvrecore/error.hpp"
#include "volk.h"
#include "vk_mem_alloc.h"
#include "core/foundation/core/foundation.h"

namespace psvrecore::vulkan {

struct DeviceOptions {
    bool validation = false;  // the Khronos validation layer, when installed; its errors are logged and counted
    i32 gpu = -1;             // the physical device's index (-1: a discrete GPU first, then any)
    nx::vector<const char*> instance_extensions;  // (e.g. a window system's surface extensions)
    nx::vector<const char*> device_extensions;
};

class Device {
public:
    static Result<nx::unique_ptr<Device>> create(const DeviceOptions& options = {});
    Device() = default;  // (empty: create() makes them)
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    u32 queue_family() const { return queue_family_; }
    VmaAllocator allocator() const { return allocator_; }
    const VkPhysicalDeviceProperties& properties() const { return properties_; }
    bool int64() const { return int64_; }  // shaderInt64
    // Errors the validation layer reported (0 without the layer).
    u32 validation_errors() const { return validation_errors_; }

    // All physical devices' names, in index order.
    static Result<nx::vector<nx::string>> list();

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT types,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    bool int64_ = false;
    u32 validation_errors_ = 0;
};

// A buffer in host-visible memory, mapped.
struct Buffer {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;
};

Result<Buffer> create_host_buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage);
void destroy_buffer(Device& device, Buffer& buffer);
// Makes the device's writes visible to the host (for memory that is not coherent).
void invalidate(Device& device, const Buffer& buffer);
void flush(Device& device, const Buffer& buffer);

// Runs a compute shader once and waits: binding i of descriptor set 0 is the storage buffer buffers[i]; `groups`
// workgroups along x.
Result<void> run_compute(Device& device, std::span<const u32> spirv, std::span<const Buffer* const> buffers,
                         u32 groups);

nx::string result_name(VkResult r);

}  // namespace psvrecore::vulkan
