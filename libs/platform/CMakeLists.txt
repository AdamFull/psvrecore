# The platform layer: a window with a Vulkan swapchain showing the frames a game displays, its audio ports played on
# the host's audio device, and the host's controllers, keyboard, mouse and touch screen fed to the runtime's input.

# SDL3 (github.com/libsdl-org/SDL, zlib license, submodule in third_party/SDL), built static unless the including
# project provides it.
if(NOT TARGET SDL3::SDL3)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory("${PSVRECORE_DIR}/third_party/SDL" SDL EXCLUDE_FROM_ALL)
    # (its Android sensor code calls an NDK function deprecated since API 26)
    if(ANDROID AND TARGET SDL3-static)
        target_compile_options(SDL3-static PRIVATE -Wno-deprecated-declarations)
    endif()
endif()

add_library(psvrecore_platform STATIC
    src/audio.cpp
    src/input.cpp
    src/window.cpp
)
add_library(psvrecore::platform ALIAS psvrecore_platform)
target_include_directories(psvrecore_platform PUBLIC include)
target_link_libraries(psvrecore_platform PUBLIC psvrecore::runtime psvrecore::gxm_hle psvrecore::vulkan SDL3::SDL3)
psvrecore_target_defaults(psvrecore_platform)
