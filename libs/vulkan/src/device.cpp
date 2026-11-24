#include "psvrecore/vulkan/device.hpp"

#include <cstdlib>
#include <cstring>

#include "psvrecore/log.hpp"
#include "psvrecore/profile.hpp"
#include "psvrecore/vulkan/renderdoc.hpp"
#include "core/foundation/strings/format.h"

namespace psvrecore::vulkan {

namespace {
// (the host buffers' usage and allocation: their memory type is found once, for the reserved memory)
constexpr VkBufferUsageFlags kHostBufferUsage =
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
constexpr VmaAllocationCreateFlags kHostBufferFlags =
    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
}  // namespace

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

// The Vulkan version an installed layer was written for (its specVersion), or 0 when it is not installed.
u32 layer_version(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceLayerProperties(&n, nullptr);
    nx::vector<VkLayerProperties> layers(n);
    vkEnumerateInstanceLayerProperties(&n, layers.data());
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return l.specVersion ? l.specVersion : 1u;
    return 0;
}

bool instance_extension(const char* name) {
    u32 n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    nx::vector<VkExtensionProperties> all(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, all.data());
    for (const auto& e : all)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

// Whether the commands get debug labels (DeviceOptions::labels).
bool labels_wanted(const DeviceOptions& options) {
    const bool renderdoc = renderdoc::loaded();  // (looked for at the start in any case: its capture keys set then)
    if (options.labels || options.validation || profile::kEnabled || renderdoc) return true;
    const char* v = std::getenv("PSVRECORE_GPU_LABELS");
    return v && *v && *v != '0';
}

// The validation layer's version, when it is on (0: off). `labels`: debug labels wanted, and then whether the
// instance has them.
Result<VkInstance> make_instance(const DeviceOptions& options, bool& validation, u32* validation_version = nullptr,
                                 bool* labels = nullptr) {
    if (const VkResult r = volkInitialize(); r != VK_SUCCESS) return nx::err(vk_error("no Vulkan loader", r));
    const u32 loader = volkGetInstanceVersion();
    if (loader < VK_API_VERSION_1_1) return failure("the Vulkan loader is older than 1.1");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "psvrecore";
    app.pEngineName = "psvrecore";
    app.apiVersion = loader >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : VK_API_VERSION_1_1;
    nx::vector<const char*> layers, extensions = options.instance_extensions;
    const u32 layer = options.validation ? layer_version("VK_LAYER_KHRONOS_validation") : 0;
    validation = layer != 0;
    if (validation_version) *validation_version = layer;
    if (options.validation && !validation) nx::logw(logs::gpu, "the Vulkan validation layer is not installed");
    if (validation) {
        layers.push_back("VK_LAYER_KHRONOS_validation");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if (labels) {
        if (*labels && !validation) *labels = instance_extension(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        if (*labels && !validation) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        *labels = *labels || validation;
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

void Device::replace_surface(VkSurfaceKHR surface) {
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = surface;
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
    u32 validation_version = 0;
    bool labels = labels_wanted(options);
    auto instance = make_instance(options, validation, &validation_version, &labels);
    if (!instance) return nx::err(instance.error());
    d->instance_ = *instance;
    d->caps_.labels = labels && vkCmdBeginDebugUtilsLabelEXT;
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
    // (as a C string: libc++'s std::format prints a char array whole, its NULs too, which ends a log line there)
    const char* const name = props.deviceName;
    if (props.apiVersion < VK_API_VERSION_1_1) return failure(nx::format("{} does not support Vulkan 1.1", name));
    const i32 family = queue_family_of(gpu, d->surface_);
    if (family < 0) return failure(nx::format("{} has no graphics and compute queue that presents", name));
    d->queue_family_ = static_cast<u32>(family);
    Capabilities& caps = d->caps_;
    caps.vendor_id = props.vendorID;
    u32 queues = 1;  // (the family's, up to two: the second presents)
    {
        u32 qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
        nx::vector<VkQueueFamilyProperties> families(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, families.data());
        if (families[d->queue_family_].timestampValidBits) caps.timestamp_period = props.limits.timestampPeriod;
        queues = nx::min<u32>(families[d->queue_family_].queueCount, 2);
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

    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gpl{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    if (has_ext(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) && has_ext(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &gpl;
        vkGetPhysicalDeviceFeatures2(gpu, &query);
        if (gpl.graphicsPipelineLibrary) {
            gpl = VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
            gpl.graphicsPipelineLibrary = VK_TRUE;
            gpl.pNext = chain;
            chain = &gpl;
            extensions.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
            extensions.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
            caps.pipeline_library = true;
            caps.dynamic_state = core13;
        }
    }

    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR local_read{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    const char* local_read_env = std::getenv("PSVRECORE_LOCAL_READ");
    // (a validation layer older than the extension (Vulkan 1.3.276) reports its structures and what it allows as
    // errors: with one, the device goes without it, as on a device that lacks it, and validation stays meaningful)
    const bool layer_knows_local_read = !validation || validation_version >= VK_MAKE_API_VERSION(0, 1, 3, 276);
    if (caps.dynamic_rendering && has_ext(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME) && !layer_knows_local_read)
        nx::logi(logs::gpu, "Vulkan device: local read left off: the validation layer ({}.{}.{}) predates it",
                 VK_API_VERSION_MAJOR(validation_version), VK_API_VERSION_MINOR(validation_version),
                 VK_API_VERSION_PATCH(validation_version));
    // (and none with rasterization-order attachments: a pass that reads its colour then takes a render pass, where the
    // order holds. The Adreno 830's driver doesn't keep it for local reads: reads see stale colour, Killzone's menus.)
    if (caps.dynamic_rendering && has_ext(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME) &&
        layer_knows_local_read && !caps.raster_order_attachment && !(local_read_env && std::atoi(local_read_env) == 0)) {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        query.pNext = &local_read;
        vkGetPhysicalDeviceFeatures2(gpu, &query);
        if (local_read.dynamicRenderingLocalRead) {
            local_read = VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR{
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
            local_read.dynamicRenderingLocalRead = VK_TRUE;
            local_read.pNext = chain;
            chain = &local_read;
            extensions.push_back(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
            caps.local_read = true;
        }
    }

#if PSVRECORE_TRACY
    // (Tracy's GPU zones on the CPU's timeline: the GPU's clock read with the host's)
    const char* calibrated = has_ext(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)   ? VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                             : has_ext(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) ? VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME
                                                                                   : nullptr;
    if (calibrated) extensions.push_back(calibrated);
#endif

    const f32 priorities[2] = {1.0f, 1.0f};
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = d->queue_family_;
    queue.queueCount = queues;
    queue.pQueuePriorities = priorities;
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
#if PSVRECORE_TRACY
    if (calibrated && std::strcmp(calibrated, VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) == 0) {
        caps.time_domains = vkGetPhysicalDeviceCalibrateableTimeDomainsKHR;
        caps.calibrated_timestamps = vkGetCalibratedTimestampsKHR;
    } else if (calibrated) {
        caps.time_domains = vkGetPhysicalDeviceCalibrateableTimeDomainsEXT;
        caps.calibrated_timestamps = vkGetCalibratedTimestampsEXT;
    }
    if (!caps.time_domains || !caps.calibrated_timestamps) caps.time_domains = nullptr, caps.calibrated_timestamps = nullptr;
#endif
    if (caps.labels) nx::logi(logs::gpu, "Vulkan device: debug labels on");
    vkGetDeviceQueue(d->device_, d->queue_family_, 0, &d->queue_);
    d->present_queue_ = d->queue_;
    if (queues > 1) vkGetDeviceQueue(d->device_, d->queue_family_, 1, &d->present_queue_);
    nx::logi(logs::gpu, "Vulkan device: presenting on {}", queues > 1 ? "a second queue" : "the one queue");
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
    {
        // (sampled and filtered, and copied into: required of these by Vulkan, BC's where it has textureCompressionBC)
        const auto sampled = [&](VkFormat f) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(gpu, f, &fp);
            const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                              VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                                              VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            return (fp.optimalTilingFeatures & need) == need;
        };
        caps.texture_srgb = sampled(VK_FORMAT_R8G8B8A8_SRGB) && sampled(VK_FORMAT_B8G8R8A8_SRGB);
        if (caps.texture_bc)
            caps.texture_srgb = caps.texture_srgb && sampled(VK_FORMAT_BC1_RGBA_SRGB_BLOCK) &&
                                sampled(VK_FORMAT_BC2_SRGB_BLOCK) && sampled(VK_FORMAT_BC3_SRGB_BLOCK);
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
    // (each block of device memory: made while drawing, it may cost a frame)
    static const VmaDeviceMemoryCallbacks memory_log{
        [](VmaAllocator, uint32_t type, VkDeviceMemory, VkDeviceSize size, void*) {
            if (size >= 16ull << 20) nx::logi(logs::gpu, "Vulkan memory: a block of {} MiB of type {} made", size >> 20, type);
            else nx::logd(logs::gpu, "Vulkan memory: {} KiB of type {} allocated", size >> 10, type);
        },
        [](VmaAllocator, uint32_t type, VkDeviceMemory, VkDeviceSize size, void*) {
            if (size >= 16ull << 20) nx::logi(logs::gpu, "Vulkan memory: a block of {} MiB of type {} freed", size >> 20, type);
        },
        nullptr};
    allocator.pDeviceMemoryCallbacks = &memory_log;
    if (const VkResult r = vmaCreateAllocator(&allocator, &d->allocator_); r != VK_SUCCESS)
        return nx::err(vk_error("vmaCreateAllocator", r));
    // The textures' and host buffers' memory, made now (blocks of 64 MiB, more when they fill up): a song of Persona 4:
    // Dancing All Night made a second block of each while it played, of 64 MiB after one of 32; with textures kept a
    // minute (gxm's evict_textures), a third for textures at a song's start.
    {
        constexpr VkDeviceSize kBlock = 64ull << 20;
        constexpr u32 kTextureBlocks = 3, kHostBlocks = 2;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = {256, 256, 1};
        image.mipLevels = image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer.size = 65536;
        buffer.usage = kHostBufferUsage;
        VmaAllocationCreateInfo texture_alloc{}, host_alloc{};
        texture_alloc.usage = host_alloc.usage = VMA_MEMORY_USAGE_AUTO;
        host_alloc.flags = kHostBufferFlags;
        u32 texture_type = 0, host_type = 0;
        VmaPoolCreateInfo pool{};
        pool.blockSize = kBlock;
        pool.minBlockCount = kTextureBlocks;
        if (vmaFindMemoryTypeIndexForImageInfo(d->allocator_, &image, &texture_alloc, &texture_type) == VK_SUCCESS) {
            pool.memoryTypeIndex = texture_type;
            if (vmaCreatePool(d->allocator_, &pool, &d->texture_pool_) != VK_SUCCESS) d->texture_pool_ = VK_NULL_HANDLE;
        }
        if (vmaFindMemoryTypeIndexForBufferInfo(d->allocator_, &buffer, &host_alloc, &host_type) == VK_SUCCESS) {
            pool.memoryTypeIndex = host_type;
            pool.minBlockCount = kHostBlocks;
            if (vmaCreatePool(d->allocator_, &pool, &d->host_pool_) != VK_SUCCESS) d->host_pool_ = VK_NULL_HANDLE;
        }
        nx::logi(logs::gpu, "Vulkan memory: {} MiB reserved for textures, {} MiB for host buffers",
                 d->texture_pool_ ? kBlock * kTextureBlocks >> 20 : 0, d->host_pool_ ? kBlock * kHostBlocks >> 20 : 0);
    }
    nx::logi(logs::gpu, "Vulkan device: {} (API {}.{}.{}, using {}.{}); {}{}{}{}{}", name,
             VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
             VK_API_VERSION_PATCH(props.apiVersion), VK_API_VERSION_MAJOR(caps.api_version),
             VK_API_VERSION_MINOR(caps.api_version), caps.dynamic_rendering ? "dynamic rendering" : "render passes",
             caps.raster_order_attachment ? ", rasterization-order attachments" : "", caps.int64 ? ", int64" : "",
             caps.pipeline_library ? ", pipeline libraries" : "", caps.local_read ? ", local read" : "",
             d->messenger_ ? "; validation layer on" : "");
    // (what the device offers that faster paths could use: not enabled)
    nx::string offered;
    for (const char* name : {"VK_EXT_external_memory_host", "VK_KHR_dynamic_rendering_local_read", "VK_EXT_shader_tile_image",
                             "VK_EXT_graphics_pipeline_library", "VK_EXT_extended_dynamic_state3",
                             "VK_ARM_rasterization_order_attachment_access"})
        if (has_ext(name)) offered += nx::format("{}{}", offered.empty() ? "" : ", ", name);
    nx::logi(logs::gpu, "Vulkan device: also offers {}", offered.empty() ? nx::string("none of the optional ones") : offered);
    return d;
}

Device::~Device() {
    if (device_) wait_idle();
    for (const SpareShot& s : spare_shots_) {
        if (s.fence) vkDestroyFence(device_, s.fence, nullptr);
        vkDestroyCommandPool(device_, s.pool, nullptr);
    }
    if (texture_pool_) vmaDestroyPool(allocator_, texture_pool_);
    if (host_pool_) vmaDestroyPool(allocator_, host_pool_);
    if (allocator_) vmaDestroyAllocator(allocator_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool Device::take_shot(SpareShot& out) {
    const nx::scoped_lock<nx::mutex> held(spare_lock_);
    if (spare_shots_.empty()) return false;
    out = spare_shots_.back();
    spare_shots_.pop_back();
    return true;
}

void Device::give_shot(const SpareShot& shot) {
    {
        const nx::scoped_lock<nx::mutex> held(spare_lock_);
        if (spare_shots_.size() < 32) {
            spare_shots_.push_back(shot);
            return;
        }
    }
    if (shot.fence) vkDestroyFence(device_, shot.fence, nullptr);
    vkDestroyCommandPool(device_, shot.pool, nullptr);
}

VkResult Device::submit(const VkSubmitInfo& info, VkFence fence) {
    const nx::scoped_lock<nx::mutex> held(queue_lock_);
    return vkQueueSubmit(queue_, 1, &info, fence);
}

void Device::wait_idle() {
    const nx::scoped_lock<nx::mutex> held(queue_lock_);
    const nx::scoped_lock<nx::mutex> present_held(present_lock_);
    vkDeviceWaitIdle(device_);
}

VkResult Device::present(const VkPresentInfoKHR& info) {
    const nx::scoped_lock<nx::mutex> held(present_queue_ == queue_ ? queue_lock_ : present_lock_);
    return vkQueuePresentKHR(present_queue_, &info);
}

Result<Buffer> create_host_buffer(Device& device, VkDeviceSize size, VkBufferUsageFlags usage) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size ? size : 4;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo alloc{};
    alloc.usage = VMA_MEMORY_USAGE_AUTO;
    alloc.flags = kHostBufferFlags;
    alloc.pool = device.host_pool();
    Buffer b;
    VmaAllocationInfo out{};
    VkResult r = vmaCreateBuffer(device.allocator(), &info, &alloc, &b.buffer, &b.allocation, &out);
    // (not in the reserved memory: too big for its blocks, or a usage its memory type does not serve)
    if (r != VK_SUCCESS && alloc.pool) {
        alloc.pool = VK_NULL_HANDLE;
        r = vmaCreateBuffer(device.allocator(), &info, &alloc, &b.buffer, &b.allocation, &out);
    }
    if (r != VK_SUCCESS) return nx::err(vk_error("vmaCreateBuffer", r));
    b.mapped = out.pMappedData;
    b.size = info.size;
    return b;
}

void destroy_buffer(Device& device, Buffer& buffer) {
    if (buffer.buffer) vmaDestroyBuffer(device.allocator(), buffer.buffer, buffer.allocation);
    buffer = Buffer{};
}

void invalidate(Device& device, const Buffer& buffer, VkDeviceSize bytes) {
    vmaInvalidateAllocation(device.allocator(), buffer.allocation, 0, bytes >= buffer.size ? VK_WHOLE_SIZE : bytes);
}

void flush(Device& device, const Buffer& buffer, VkDeviceSize bytes) {
    vmaFlushAllocation(device.allocator(), buffer.allocation, 0, bytes >= buffer.size ? VK_WHOLE_SIZE : bytes);
}

namespace {
VkDebugUtilsLabelEXT label_of(const char* text, u32 rgb) {
    VkDebugUtilsLabelEXT l{VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT};
    l.pLabelName = text;
    if (rgb) {
        l.color[0] = static_cast<f32>((rgb >> 16) & 0xFF) / 255.0f;
        l.color[1] = static_cast<f32>((rgb >> 8) & 0xFF) / 255.0f;
        l.color[2] = static_cast<f32>(rgb & 0xFF) / 255.0f;
        l.color[3] = 1.0f;
    }
    return l;
}
}  // namespace

void Device::begin_label(VkCommandBuffer cmd, const char* text, u32 rgb) const {
    if (!caps_.labels) return;
    const VkDebugUtilsLabelEXT l = label_of(text, rgb);
    vkCmdBeginDebugUtilsLabelEXT(cmd, &l);
}

void Device::end_label(VkCommandBuffer cmd) const {
    if (caps_.labels) vkCmdEndDebugUtilsLabelEXT(cmd);
}

void Device::insert_label(VkCommandBuffer cmd, const char* text, u32 rgb) const {
    if (!caps_.labels) return;
    const VkDebugUtilsLabelEXT l = label_of(text, rgb);
    vkCmdInsertDebugUtilsLabelEXT(cmd, &l);
}

void Device::name_object(VkObjectType type, u64 handle, const char* name) const {
    if (!caps_.labels || !handle || !vkSetDebugUtilsObjectNameEXT) return;
    VkDebugUtilsObjectNameInfoEXT info{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    info.objectType = type;
    info.objectHandle = handle;
    info.pObjectName = name;
    vkSetDebugUtilsObjectNameEXT(device_, &info);
}

}  // namespace psvrecore::vulkan
