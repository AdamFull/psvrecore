add_executable(psvusse main.cpp gpu.cpp)
target_link_libraries(psvusse PRIVATE psvrecore::usse psvrecore::usse_interp psvrecore::gxm psvrecore::vulkan)
psvrecore_target_defaults(psvusse)
