#pragma once

// The Vulkan device: an instance (with the validation layer on request), the physical device chosen, a logical
// device with one queue that does graphics and compute, and a VMA allocator. Vulkan 1.1 is the minimum; 1.3 is used
// where the driver has it. What the GXM layer can use beyond 1.1 is enabled when present and reported in
// Capabilities (after psprecore's display, which runs on the same range of drivers).

#include <span>

#include "psvrecore/error.hpp"
#include "volk.h"
#include "vk_mem_alloc.h"
#include "core/foundation/core/foundation.h"
#include "core/foundation/threading/sync.h"

namespace psvrecore::vulkan {

struct DeviceOptions {
    bool validation = false;  // the Khronos validation layer, when installed; its errors are logged and counted
    i32 gpu = -1;             // the physical device's index (-1: a discrete GPU first, then any)
    bool render_passes = false;  // draw with render passes even where dynamic rendering exists (to test that path)
    nx::vector<const char*> instance_extensions;  // (e.g. a window system's surface extensions)
    // Called with the instance before the device is chosen; the device must then present to the surface it returns
    // (VK_NULL_HANDLE: no presenting, no swapchain extension).
    VkSurfaceKHR (*make_surface)(VkInstance instance, void* user) = nullptr;
    void* surface_user = nullptr;
};

// What the device has, beyond Vulkan 1.1.
struct Capabilities {
    u32 api_version = 0;          // the version used: min(1.3, the device's)
    bool dynamic_rendering = false;  // core 1.3 or VK_KHR_dynamic_rendering (else render passes)
    bool int64 = false;           // shaderInt64
    bool logic_op = false;
    bool sampler_anisotropy = false;
    bool depth_clamp = false;
    bool depth_bias_clamp = false;
    bool wide_lines = false;
    bool texture_bc = false;
    bool texture_astc = false;
    bool texture_etc2 = false;
    bool occlusion_query_precise = false;  // (else occlusion queries may count 1 for any number of samples)
    // Reading the destination colour in the fragment shader (GXM's programmable blending): input attachments
    // with rasterization-order access (VK_EXT_ or VK_ARM_rasterization_order_attachment_access).
    bool raster_order_attachment = false;
    VkFormat depth_stencil_format = VK_FORMAT_UNDEFINED;  // D32_SFLOAT_S8_UINT or D24_UNORM_S8_UINT
    u32 vendor_id = 0;
    f32 timestamp_period = 0.0f;  // nanoseconds per timestamp tick (0: the queue has no timestamps)
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
    VkSurfaceKHR surface() const { return surface_; }
    VmaAllocator allocator() const { return allocator_; }
    const VkPhysicalDeviceProperties& properties() const { return properties_; }
    const Capabilities& caps() const { return caps_; }
    bool int64() const { return caps_.int64; }  // shaderInt64
    // Held around every use of the queue (submits, presents, waiting for it to be idle): the GXM layer and the
    // display may use it from different threads.
    nx::mutex& queue_lock() { return queue_lock_; }
    // Errors the validation layer reported (0 without the layer).
    u32 validation_errors() const { return validation_errors_; }

    // Submits under the queue lock.
    VkResult submit(const VkSubmitInfo& info, VkFence fence);
    void wait_idle();

    // Dynamic rendering's entry points (core or KHR), null without it.
    PFN_vkCmdBeginRendering begin_rendering = nullptr;
    PFN_vkCmdEndRendering end_rendering = nullptr;

    // All physical devices' names, in index order.
    static Result<nx::vector<nx::string>> list();

private:
    static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT types,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    Capabilities caps_;
    nx::mutex queue_lock_;
    u32 validation_errors_ = 0;
};

nx::string result_name(VkResult r);
// A failure naming the call and its result.
Error vk_error(const char* what, VkResult r);

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

}  // namespace psvrecore::vulkan
