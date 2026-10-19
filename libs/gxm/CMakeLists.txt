# GXM on Vulkan: the USSE to SPIR-V shader translator (and, later, the GXM API's state and draws).
add_library(psvrecore_gxm STATIC
    src/spirv.cpp
    src/translate.cpp
)
add_library(psvrecore::gxm ALIAS psvrecore_gxm)

target_include_directories(psvrecore_gxm
    PUBLIC include
    # SPIRV-Headers (github.com/KhronosGroup/SPIRV-Headers, MIT-style, submodule in third_party/SPIRV-Headers)
    PRIVATE "${PSVRECORE_DIR}/third_party/SPIRV-Headers/include")
target_link_libraries(psvrecore_gxm PUBLIC psvrecore::usse)
psvrecore_target_defaults(psvrecore_gxm)

# The GXM and display system libraries on the runtime (HLE): what games call.
add_library(psvrecore_gxm_hle STATIC
    src/hle/context.cpp
    src/hle/core.cpp
    src/hle/display.cpp
    src/hle/patcher.cpp
    src/hle/render.cpp
    src/hle/surfaces.cpp
)
add_library(psvrecore::gxm_hle ALIAS psvrecore_gxm_hle)
target_include_directories(psvrecore_gxm_hle PRIVATE "${PSVRECORE_DIR}/third_party/SPIRV-Headers/include")
target_link_libraries(psvrecore_gxm_hle PUBLIC psvrecore::gxm psvrecore::vulkan psvrecore::runtime)
psvrecore_target_defaults(psvrecore_gxm_hle)
