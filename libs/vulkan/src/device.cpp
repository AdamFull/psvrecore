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
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        default: return nx::format("VkResult {}", static_cast<i32>(r));
    }
}

namespace {

Error vk_error(const char* what, VkResult r) { return Error{nx::format("{}: {}", what, result_name(r))}; }

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
    if (volkGetInstanceVersion() < VK_API_VERSION_1_1) return failure("the Vulkan loader is older than 1.1");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "psvrecore";
    app.pEngineName = "psvrecore";
    app.apiVersion = VK_API_VERSION_1_1;
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

// The first queue family that does graphics and compute.
i32 queue_family_of(VkPhysicalDevice p) {
    u32 n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(p, &n, nullptr);
    nx::vector<VkQueueFamilyProperties> families(n);
    vkGetPhysicalDeviceQueueFamilyProperties(p, &n, families.data());
    for (u32 i = 0; i < n; ++i)
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))
            return static_cast<i32>(i);
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
        info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        info.pfnUserCallback = on_message;
        info.pUserData = d.get();
        vkCreateDebugUtilsMessengerEXT(d->instance_, &info, nullptr, &d->messenger_);
    }

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
        // A discrete GPU first, then an integrated one, then anything with a queue for graphics and compute.
        for (const VkPhysicalDeviceType want : {VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU, VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU,
                                                VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM}) {
            for (u32 i = 0; i < n && chosen < 0; ++i) {
                VkPhysicalDeviceProperties props;
                vkGetPhysicalDeviceProperties(devices[i], &props);
                if (props.apiVersion < VK_API_VERSION_1_1 || queue_family_of(devices[i]) < 0) continue;
                if (want == VK_PHYSICAL_DEVICE_TYPE_MAX_ENUM || props.deviceType == want) chosen = static_cast<i32>(i);
            }
        }
        if (chosen < 0) return failure("no Vulkan 1.1 device with a graphics and compute queue");
    }
    d->physical_ = devices[static_cast<u32>(chosen)];
    vkGetPhysicalDeviceProperties(d->physical_, &d->properties_);
    if (d->properties_.apiVersion < VK_API_VERSION_1_1)
        return failure(nx::format("{} does not support Vulkan 1.1", d->properties_.deviceName));
    const i32 family = queue_family_of(d->physical_);
    if (family < 0) return failure(nx::format("{} has no graphics and compute queue", d->properties_.deviceName));
    d->queue_family_ = static_cast<u32>(family);

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(d->physical_, &supported);
    VkPhysicalDeviceFeatures features{};
    features.shaderInt64 = supported.shaderInt64;
    d->int64_ = supported.shaderInt64 == VK_TRUE;

    const f32 priority = 1.0f;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = d->queue_family_;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = static_cast<u32>(options.device_extensions.size());
    info.ppEnabledExtensionNames = options.device_extensions.data();
    info.pEnabledFeatures = &features;
    if (const VkResult r = vkCreateDevice(d->physical_, &info, nullptr, &d->device_); r != VK_SUCCESS)
        return nx::err(vk_error("vkCreateDevice", r));
    volkLoadDevice(d->device_);
    vkGetDeviceQueue(d->device_, d->queue_family_, 0, &d->queue_);

    VmaVulkanFunctions functions{};
    VmaAllocatorCreateInfo allocator{};
    allocator.vulkanApiVersion = VK_API_VERSION_1_1;
    allocator.physicalDevice = d->physical_;
    allocator.device = d->device_;
    allocator.instance = d->instance_;
    if (const VkResult r = vmaImportVulkanFunctionsFromVolk(&allocator, &functions); r != VK_SUCCESS)
        return nx::err(vk_error("vmaImportVulkanFunctionsFromVolk", r));
    allocator.pVulkanFunctions = &functions;
    if (const VkResult r = vmaCreateAllocator(&allocator, &d->allocator_); r != VK_SUCCESS)
        return nx::err(vk_error("vmaCreateAllocator", r));
    nx::logi(logs::gpu, "Vulkan device: {} (API {}.{}.{})", d->properties_.deviceName,
             VK_API_VERSION_MAJOR(d->properties_.apiVersion), VK_API_VERSION_MINOR(d->properties_.apiVersion),
             VK_API_VERSION_PATCH(d->properties_.apiVersion));
    return d;
}

Device::~Device() {
    if (device_) vkDeviceWaitIdle(device_);
    if (allocator_) vmaDestroyAllocator(allocator_);
    if (device_) vkDestroyDevice(device_, nullptr);
    if (messenger_) vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
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
