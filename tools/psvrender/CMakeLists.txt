# The rendering tests' runner (scripts/render_check.py): the reference renderer against the Vulkan backend.
add_executable(psvrender main.cpp gpu.cpp)
target_link_libraries(psvrender PRIVATE psvrecore::gxm_reference psvrecore::gxm psvrecore::vulkan)
psvrecore_target_defaults(psvrender)
