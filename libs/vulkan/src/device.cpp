#include "psvrecore/vulkan/device.hpp"

#include <cstring>

#include "psvrecore/log.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::vulkan {

nx::string result_name(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        default: return nx::format("VkResult {}", static_cast<i32>(r));
    }
}

Error vk_error(const char* what, VkResult r) { return Error{nx::format("{}: {}", what, result_name(r))}; }

namespace {

bool has_layer(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    nx::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

Result<VkInstance> make_instance(const DeviceOptions& options, bool& validation) {
    if (const VkResult r = volkInitialize(); r != VK_SUCCESS) return nx::err(vk_error("no Vulkan loader", r));
    const u32 loader = volkGetInstanceVersion();
    if (loader < VK_API_VERSION_1_1) return failure("the Vulkan loader is older than 1.1");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "psvrecore";
    app.pEngineName = "psvrecore";
    app.apiVersion = loader >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
    nx::vector<const char*> layers, extensions = options.instance_extensions;
    validation = options.validation && has_layer("VK_LAYER_KHRONOS_validation");
    if (options.validation && !validation) nx::logw(logs::gpu, "the Vulkan validation layer is not installed");
    if (validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledLayerCount = static_cast<u32>(layers.size());
    info.ppEnabledLayerNames = layers.data();
    info.enabledExtensionCount = static_cast<u32>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    VkInstance instance = VK_NULL_HANDLE;
    if (const VkResult r = vkCreateInstance(&info, nullptr, &instance); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateInstance", r));
    volkLoadInstanceOnly(instance);
    return instance;
}

// The first queue family that does graphics and compute (and presents to the surface, when there is one).
i32 queue_family_of(VkPhysicalDevice p, VkSurfaceKHR surface) {
    u32 n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &n, nullptr);
    nx::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &n, families.data());
    for (u32 i = 0; i < n; ++i) {
        constexpr VkQueueFlags kWanted = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
        if ((families[i].queueFlags & kWanted) != kWanted) continue;
        if (surface) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(p, i, surface, &present);
            if (!present) continue;
        }
        return static_cast<i32>(i);
    }
    return -1;
}

}  // namespace

VKAPI_ATTR VkBool32 VKAPI_CALL Device::on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                  VkDebugUtilsMessageTypeFlagsEXT,
                                                  const VkDebugUtilsMessengerCallbackDataEXT* data, void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ++static_cast<Device*>(user)->validation_errors_;
        nx::loge(logs::gpu, "validation: {}", data->pMessage ? data->pMessage : "");
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        nx::logw(logs::gpu, "validation: {}", data->pMessage ? data->pMessage : "");
    }
    return VK_FALSE;
}

Result<nx::vector<nx::string>> Device::list() {
    bool validation = false;
    auto instance = make_instance({}, validation);
    if (!instance) return nx::err(instance.error());
    u32 n = 0;
    vkEnumeratePhysicalDevices(*instance, &n, nullptr);
    nx::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(*instance, &n, devices.data());
    nx::vector<nx::string> names;
    for (VkPhysicalDevice p : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(p, &props);
        names.push_back(props.deviceName);
    }
    vkDestroyInstance(*instance, nullptr);
    return names;
}

Result<nx::unique_ptr<Device>> Device::create(const DeviceOptions& options) {
    auto d = nx::make_unique<Device>();
    bool validation = false;
    auto instance = make_instance(options, validation);
    if (!instance) return nx::err(instance.error());
    d->instance_ = *instance;
    if (validation) {
        VkDebugUtilsMessengerCreateInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        info.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = on_message;
        info.pUserData = d.get();
        vkCreateDebugUtilsMessengerEXT(d->instance_, &info, nullptr, &d->messenger_);
    }
    if (options.make_surface) {
        d->surface_ = options.make_surface(d->instance_, options.surface_user);
        if (!d->surface_) return failure("no Vulkan surface for the window");
    }

    // The physical device: by index, else a discrete GPU, then an integrated one, then any.
    u32 n = 0;
    vkEnumeratePhysicalDevices(d->instance_, &n, nullptr);
    nx::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(d->instance_, &n, devices.data());
    if (devices.empty()) return failure("no Vulkan device");
    i32 chosen = -1;
    if (options.gpu >= 0) {
        if (static_cast<u32>(options.gpu) >= n) return failure(nx::format("no Vulkan device {}", options.gpu));
        chosen = options.gpu;
    } else {
        for (const VkPhysicalDeviceType want :
             {VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU, VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM}) {
            for (u32 i = 0; i < n && chosen < 0; ++i) {
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(devices[i], &props);
                if (props.apiVersion < VK_API_VERSION_1_1 || queue_family_of(devices[i], d->surface_) < 0) continue;
                if (want == VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM || props.deviceType == want) chosen = static_cast<i32>(i);
            }
        }
        if (chosen < 0) return failure("no Vulkan 1.1 device with a graphics and compute queue");
    }
    d->physical_ = devices[static_cast<u32>(chosen)];
    VkPhysicalDevice gpu = d->physical_;
    vkGetPhysicalDeviceProperties(gpu, &d->properties_);
    const VkPhysicalDeviceProperties& props = d->properties_;
    if (props.apiVersion < VK_API_VERSION_1_1) return failure(nx::format("{} does not support Vulkan 1.1", props.deviceName));
    const i32 family = queue_family_of(gpu, d->surface_);
    if (family < 0) return failure(nx::format("{} has no graphics and compute queue that presents", props.deviceName));
    d->queue_family_ = static_cast<u32>(family);
    Capabilities& caps = d->caps_;
    caps.vendor_id = props.vendorID;
    {
        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
        nx::vector<VkQueueFamilyProperties> families(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, families.data());
        if (families[d->queue_family_].timestampValidBits) caps.timestamp_period = props.limits.timestampPeriod;
    }

    // Extensions: the swapchain when presenting; dynamic rendering where 1.3 does not have it; rasterization-order
    // attachment access.
    caps.api_version = nx::min<u32>(VK_API_VERSION_1_3, props.apiVersion);
    if (volkGetInstanceVersion() < VK_API_VERSION_1_3) caps.api_version = nx::min<u32>(caps.api_version, VK_API_VERSION_1_1);
    u32 ext_count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, nullptr);
    nx::vector<VkExtensionProperties> available(ext_count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &ext_count, available.data());
    auto has_ext = [&](const char* name) {
        for (const auto& e : available)
            if (std::strcmp(e.extensionName, name) == 0) return true;
        return false;
    };
    nx::vector<const char*> extensions;
    if (d->surface_) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    const bool core13 = caps.api_version >= VK_API_VERSION_1_3;
    const bool dynamic_ext = !core13 && has_ext(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) &&
                             has_ext(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME) &&
                             has_ext(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    if (dynamic_ext) {
        extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        extensions.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
        extensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
    }
    caps.dynamic_rendering = (core13 || dynamic_ext) && !options.render_passes;
    const char* raster_order = has_ext(VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)
                                   ? VK_EXT_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME
                               : has_ext(VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME)
                                   ? VK_ARM_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_EXTENSION_NAME
                                   : nullptr;

    // Features: only what is used, where supported.
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(gpu, &supported);
    VkPhysicalDeviceFeatures enabled{};
    enabled.shaderInt64 = supported.shaderInt64;
    enabled.logicOp = supported.logicOp;
    enabled.samplerAnisotropy = supported.samplerAnisotropy;
    enabled.depthClamp = supported.depthClamp;
    enabled.depthBiasClamp = supported.depthBiasClamp;
    enabled.wideLines = supported.wideLines;
    enabled.textureCompressionBC = supported.textureCompressionBC;
    enabled.textureCompressionASTC_LDR = supported.textureCompressionASTC_LDR;
    enabled.textureCompressionETC2 = supported.textureCompressionETC2;
    enabled.occlusionQueryPrecise = supported.occlusionQueryPrecise;
    enabled.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
    enabled.fragmentStoresAndAtomics = supported.fragmentStoresAndAtomics;
    caps.int64 = enabled.shaderInt64;
    caps.logic_op = enabled.logicOp;
    caps.sampler_anisotropy = enabled.samplerAnisotropy;
    caps.depth_clamp = enabled.depthClamp;
    caps.depth_bias_clamp = enabled.depthBiasClamp;
    caps.wide_lines = enabled.wideLines;
    caps.texture_bc = enabled.textureCompressionBC;
    caps.texture_astc = enabled.textureCompressionASTC_LDR;
    caps.texture_etc2 = enabled.textureCompressionETC2;
    caps.occlusion_query_precise = enabled.occlusionQueryPrecise;
    caps.shader_stores = enabled.vertexPipelineStoresAndAtomics && enabled.fragmentStoresAndAtomics;

    void* chain = nullptr;
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynamic_khr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    if (core13) {
        features13.dynamicRendering = VK_TRUE;
        chain = &features13;
    } else if (dynamic_ext) {
        dynamic_khr.dynamicRendering = VK_TRUE;
        chain = &dynamic_khr;
    }
    VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT order{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
    if (raster_order) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &order;
        vkGetPhysicalDeviceFeatures2(gpu, &query);
        if (order.rasterizationOrderColorAttachmentAccess) {
            order = VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RASTERIZATION_ORDER_ATTACHMENT_ACCESS_FEATURES_EXT};
            order.rasterizationOrderColorAttachmentAccess = VK_TRUE;
            order.pNext = chain;
            chain = &order;
            extensions.push_back(raster_order);
            caps.raster_order_attachment = true;
        }
    }

    const f32 priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = d->queue_family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.pNext = chain;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<u32>(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.pEnabledFeatures = &enabled;
    if (const VkResult r = vkCreateDevice(gpu, &info, nullptr, &d->device_); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateDevice", r));
    volkLoadDevice(d->device_);
    vkGetDeviceQueue(d->device_, d->queue_family_, 0, &d->queue_);
    if (caps.dynamic_rendering) {
        d->begin_rendering = core13 ? vkCmdBeginRendering : vkCmdBeginRenderingKHR;
        d->end_rendering = core13 ? vkCmdEndRendering : vkCmdEndRenderingKHR;
    }
    for (const VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT}) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(gpu, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            caps.depth_stencil_format = f;
            break;
        }
    }

    VmaVulkanFunctions functions{};
    VmaAllocatorCreateInfo allocator{};
    allocator.vulkanApiVersion = caps.api_version;
    allocator.physicalDevice = gpu;
    allocator.device = d->device_;
    allocator.instance = d->instance_;
    if (const VkResult r = vmaImportVulkanFunctionsFromVolk(&allocator, &functions); r != VK_SUCCESS)
        return nx::err(vk_error("vmaImportVulkanFunctionsFromVolk", r));
    allocator.pVulkanFunctions = &functions;
    if (const VkResult r = vmaCreateAllocator(&allocator, &d->allocator_); r != VK_SUCCESS)
        return nx::err(vk_error("vmaCreateAllocator", r));
    nx::logi(logs::gpu, "Vulkan device: {} (API {}.{}.{}, using {}.{}); {}{}{}{}", props.deviceName,
             VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion), VK_API_VERSION_MAJOR(caps.api_version),
             VK_API_VERSION_MINOR(caps.api_version), caps.dynamic_rendering ? "dynamic rendering" : "render passes",
             caps.raster_order_attachment ? ", rasterization-order attachments" : "", caps.int64 ? ", int64" : "",
             d->messenger_ ? "; validation layer on" : "");
    return d;
}

Device::~Device() {
    if (device_) wait_idle();
    if (allocator_) vmaDestroyAllocator(allocator_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

VkResult Device::submit(const VkSubmitInfo& info, VkFence fence) {
    const nx::scoped_lock<nx::mutex> held(queue_lock_);
    return vkQueueSubmit(queue_, 1, &info, fence);
}

void Device::wait_idle() {
    const nx::scoped_lock<nx::mutex> held(queue_lock_);
    vkDeviceWaitIdle(device_);
}

Result<Buffer> create_host_buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size ? size : 4;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    Buffer b;
    VmaAllocationInfo out{};
    if (const VkResult r = vmaCreateBuffer(device.allocator(), &info, &alloc, &b.buffer, &b.allocation, &out);
        r != VK_SUCCESS)
        return nx::err(vk_error("vmaCreateBuffer", r));
    b.mapped = out.pMappedData;
    b.size = info.size;
    return b;
}

void destroy_buffer(Device& device, Buffer& buffer) {
    if (buffer.buffer) vmaDestroyBuffer(device.allocator(), buffer.buffer, buffer.allocation);
    buffer = Buffer{};
}

void invalidate(Device& device, const Buffer& buffer) {
    vmaInvalidateAllocation(device.allocator(), buffer.allocation, 0, VK_WHOLE_SIZE);
}

void flush(Device& device, const Buffer& buffer) {
    vmaFlushAllocation(device.allocator(), buffer.allocation, 0, VK_WHOLE_SIZE);
}

}  // namespace psvrecore::vulkan
