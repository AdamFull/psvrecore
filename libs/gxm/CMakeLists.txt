# GXM on Vulkan: the USSE to SPIR-V shader translator (and, later, the GXM API's state and draws).
add_library(psvrecore_gxm STATIC
    src/spirv.cpp
    src/texture.cpp
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
    src/hle/precomputed.cpp
    src/hle/render.cpp
    src/hle/shader_cache.cpp
    src/hle/surfaces.cpp
    src/hle/textures.cpp
    src/hle/transfer.cpp
)
add_library(psvrecore::gxm_hle ALIAS psvrecore_gxm_hle)
target_include_directories(psvrecore_gxm_hle PRIVATE "${PSVRECORE_DIR}/third_party/SPIRV-Headers/include")
target_link_libraries(psvrecore_gxm_hle PUBLIC psvrecore::gxm psvrecore::vulkan psvrecore::runtime)
psvrecore_target_defaults(psvrecore_gxm_hle)
# The shader cache's translator id: a hash of the sources the SPIR-V it keeps comes from (the translator, the USSE
# decoder), so a cache an older translator wrote is not used. CMake runs again when one of them changes.
file(GLOB _translator_sources
    "${CMAKE_CURRENT_SOURCE_DIR}/src/translate.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/src/spirv.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/psvrecore/gxm/translate.hpp" "${CMAKE_CURRENT_SOURCE_DIR}/include/psvrecore/gxm/spirv.hpp"
    "${PSVRECORE_DIR}/libs/usse/src/*.cpp" "${PSVRECORE_DIR}/libs/usse/src/*.hpp"
    "${PSVRECORE_DIR}/libs/usse/include/psvrecore/usse/*.hpp")
list(SORT _translator_sources)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_translator_sources})
set(_translator_hashes "")
foreach(_source IN LISTS _translator_sources)
    file(READ "${_source}" _text)
    string(REPLACE "\r\n" "\n" _text "${_text}")  # (the same id whatever the checkout's line endings)
    string(SHA256 _hash "${_text}")
    string(APPEND _translator_hashes "${_hash}")
endforeach()
string(SHA256 _translator_id "${_translator_hashes}")
target_compile_definitions(psvrecore_gxm_hle PRIVATE PSVRECORE_TRANSLATOR_ID="${_translator_id}")

# The reference renderer (gxm/reference.hpp): the rendering tests' oracle, on the USSE reference interpreter. Tests
# and tools only: never linked into a game.
add_library(psvrecore_gxm_reference STATIC src/reference.cpp)
add_library(psvrecore::gxm_reference ALIAS psvrecore_gxm_reference)
target_link_libraries(psvrecore_gxm_reference PUBLIC psvrecore::gxm psvrecore::usse_interp)
psvrecore_target_defaults(psvrecore_gxm_reference)
