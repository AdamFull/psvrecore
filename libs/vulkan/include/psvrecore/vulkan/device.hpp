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
    // Debug labels (VK_EXT_debug_utils) on the commands, for a GPU debugger or profiler: also on with the validation
    // layer, when RenderDoc is in the process, in a Tracy build, and with PSVRECORE_GPU_LABELS=1 (docs/PROFILING.md).
    bool labels = false;
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
    // R8G8B8A8_SRGB and B8G8R8A8_SRGB images sampled (filtered) and uploaded to, and BC1-BC3's sRGB formats with
    // texture_bc: gamma-mode textures kept encoded, the sampler decoding them.
    bool texture_srgb = false;
    bool texture_astc = false;
    bool texture_etc2 = false;
    bool occlusion_query_precise = false;
    bool shader_stores = false;  // vertexPipelineStoresAndAtomics and fragmentStoresAndAtomics  // (else occlusion queries may count 1 for any number of samples)
    // Reading the destination colour in the fragment shader (GXM's programmable blending): input attachments
    // with rasterization-order access (VK_EXT_ or VK_ARM_rasterization_order_attachment_access).
    bool raster_order_attachment = false;
    bool pipeline_library = false;  // VK_EXT_graphics_pipeline_library
    // VK_KHR_dynamic_rendering_local_read: a pass reading its colour as an input attachment rendered dynamically too
    // (no render pass: its pipelines made of libraries as the others are). PSVRECORE_LOCAL_READ=0 turns it off.
    bool local_read = false;
    // The libraries' rasterization, depth and stencil state dynamic (Vulkan 1.3's extended dynamic state): a shader's
    // library is the same for all of them, found in the driver's cache in later runs.
    bool dynamic_state = false;
    VkFormat depth_stencil_format = VK_FORMAT_UNDEFINED;  // D32_SFLOAT_S8_UINT or D24_UNORM_S8_UINT
    bool labels = false;  // debug labels on (DeviceOptions::labels)
    // (a Tracy build: VK_KHR_ or VK_EXT_calibrated_timestamps, the GPU's clock read with the host's; null without)
    PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR time_domains = nullptr;
    PFN_vkGetCalibratedTimestampsKHR calibrated_timestamps = nullptr;
    u32 vendor_id = 0;
    f32 timestamp_period = 0.0f;  // nanoseconds per timestamp tick (0: the queue has no timestamps)
};

class Device {
public:
    static Result<nx::unique_ptr<Device>> create(const DeviceOptions& options = {});
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    VkInstance instance() const { return instance_; }
    VkPhysicalDevice physical() const { return physical_; }
    VkDevice device() const { return device_; }
    VkQueue queue() const { return queue_; }
    u32 queue_family() const { return queue_family_; }
    VkSurfaceKHR surface() const { return surface_; }
    // Another surface for the same window (the old one destroyed): on Android a window's surface goes when the
    // application is sent to the background and comes back new. VK_NULL_HANDLE: none for now.
    void replace_surface(VkSurfaceKHR surface);
    VmaAllocator allocator() const { return allocator_; }
    // The memory textures (sampled images filled by copies) and host buffers come from first, its blocks made with
    // the device (null: none; the allocator's own memory then). A phone's driver took 20 ms and more for each block
    // of 64 MiB made while a game drew, a frame and more each time.
    VmaPool texture_pool() const { return texture_pool_; }
    VmaPool host_pool() const { return host_pool_; }
    const VkPhysicalDeviceProperties& properties() const { return properties_; }
    const Capabilities& caps() const { return caps_; }
    bool int64() const { return caps_.int64; }  // shaderInt64
    // Held around every use of the queue (submits, presents, waiting for it to be idle): the GXM layer and the
    // display may use it from different threads.
    nx::mutex& queue_lock() { return queue_lock_; }
    // Errors the validation layer reported (0 without the layer).
    u32 validation_errors() const { return validation_errors_; }

    // Debug labels: a region of commands (begun and ended in the same command buffer) and a single point, named and
    // coloured (0xRRGGBB, 0: none) in a GPU debugger's list of commands. Nothing without caps().labels.
    void begin_label(VkCommandBuffer cmd, const char* text, u32 rgb = 0) const;
    void end_label(VkCommandBuffer cmd) const;
    void insert_label(VkCommandBuffer cmd, const char* text, u32 rgb = 0) const;
    // A name for an object (a debugger shows it instead of its handle). Nothing without caps().labels.
    void name_object(VkObjectType type, u64 handle, const char* name) const;

    // Submits under the queue lock.
    VkResult submit(const VkSubmitInfo& info, VkFence fence);
    // Presents on a second queue of the family where it has one, under that queue's own lock: an Adreno driver's
    // present took 7 ms and up to 24 with the frame's GPU work already done, and scenes' submits waiting for the
    // one queue's lock waited as long. On the queue where there is only one, under its lock.
    VkResult present(const VkPresentInfoKHR& info);
    void wait_idle();  // (both queues' locks held)

    // A command pool with its command buffer and a fence, reset, for OneShot: kept rather than destroyed and made
    // again for each (a scene's commands: destroying a pool took 3.5% of a phone's render thread). take_shot: false
    // when none is kept.
    struct SpareShot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;  // (unsignaled; may be none)
    };
    bool take_shot(SpareShot& out);
    void give_shot(const SpareShot& shot);  // (destroyed when enough are kept)

    // Dynamic rendering's entry points (core or KHR), null without it.
    PFN_vkCmdBeginRendering begin_rendering = nullptr;
    PFN_vkCmdEndRendering end_rendering = nullptr;

    // All physical devices' names, in index order.
    static Result<nx::vector<nx::string>> list();

private:
    template <typename T, typename... Args>
    friend nx::unique_ptr<T> nx::make_unique(Args&&...);
    Device() = default;  // (empty: create() makes them)
    static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                     VkDebugUtilsMessageTypeFlagsEXT types,
                                                     const VkDebugUtilsMessengerCallbackDataEXT* data, void* user);

    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE, present_queue_ = VK_NULL_HANDLE;
    u32 queue_family_ = 0;
    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VmaPool texture_pool_ = VK_NULL_HANDLE, host_pool_ = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties_{};
    Capabilities caps_;
    nx::mutex queue_lock_, present_lock_;
    nx::mutex spare_lock_;
    nx::vector<SpareShot> spare_shots_;
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
// Makes the device's writes visible to the host (for memory that is not coherent), and the host's to the device: the
// first `bytes` (all of it by default; a device's cache maintenance costs by the byte: a Mali driver's took 12% of a
// phone's render thread with whole buffers of which a part was used).
void invalidate(Device& device, const Buffer& buffer, VkDeviceSize bytes = VK_WHOLE_SIZE);
void flush(Device& device, const Buffer& buffer, VkDeviceSize bytes = VK_WHOLE_SIZE);

// Runs a compute shader once and waits: binding i of descriptor set 0 is the storage buffer buffers[i]; `groups`
// workgroups along x.
Result<void> run_compute(Device& device, std::span<const u32> spirv, std::span<const Buffer* const> buffers,
                         u32 groups);

}  // namespace psvrecore::vulkan
