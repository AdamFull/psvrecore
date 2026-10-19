# The rendering tests' runner (scripts/render_check.py): the reference renderer against the Vulkan backend.
add_executable(psvrender main.cpp gpu.cpp "${PSVRECORE_DIR}/tests/samples/png.cpp")
target_include_directories(psvrender PRIVATE "${PSVRECORE_DIR}/tests/samples")
target_link_libraries(psvrender PRIVATE psvrecore::gxm_reference psvrecore::gxm psvrecore::vulkan)
psvrecore_target_defaults(psvrender)
