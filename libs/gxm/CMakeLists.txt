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
