# The Vulkan device layer: instance, device and queue (volk loads the entry points), memory (VMA), buffers and
# compute dispatches. GXM renders through it.

# volk (github.com/zeux/volk, MIT, submodule in third_party/volk) with Vulkan-Headers (Apache-2.0/MIT, submodule in
# third_party/Vulkan-Headers): the Vulkan loader is opened at run time, nothing links against it.
add_library(psvrecore_volk STATIC "${PSVRECORE_DIR}/third_party/volk/volk.c")
target_include_directories(psvrecore_volk SYSTEM PUBLIC
    "${PSVRECORE_DIR}/third_party/volk"
    "${PSVRECORE_DIR}/third_party/Vulkan-Headers/include")
target_compile_definitions(psvrecore_volk PUBLIC VK_NO_PROTOTYPES)

add_library(psvrecore_vulkan STATIC
    src/compute.cpp
    src/device.cpp
    src/vma.cpp
)
add_library(psvrecore::vulkan ALIAS psvrecore_vulkan)

target_include_directories(psvrecore_vulkan PUBLIC include)
# VulkanMemoryAllocator (github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator, MIT, submodule in
# third_party/VulkanMemoryAllocator): header-only, implemented in src/vma.cpp.
target_include_directories(psvrecore_vulkan SYSTEM PUBLIC "${PSVRECORE_DIR}/third_party/VulkanMemoryAllocator/include")
target_link_libraries(psvrecore_vulkan PUBLIC psvrecore::core psvrecore_volk)
psvrecore_target_defaults(psvrecore_vulkan)
# (VMA's implementation is third-party code: its warnings are not ours)
set_source_files_properties(src/vma.cpp PROPERTIES COMPILE_OPTIONS "-w")
