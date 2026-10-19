cmake_minimum_required(VERSION 3.25)

# A toolkit for static recompilation of PS Vita software: SELF/ELF analysis, the ARMv7/Thumb-2 recompiler to C, and
# the runtime the recompiled code runs on (HLE system libraries, GXM on Vulkan, audio, input, files, platform layer).
# Built on its own (tools and tests) or by a game repository through add_subdirectory.
project(psvrecore
    VERSION 0.1.0
    DESCRIPTION "Static recompilation of PS Vita software to native code"
    LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(PSVRECORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE INTERNAL "psvrecore source directory")

# Host tools and tests are not built for Android.
if(ANDROID)
    set(_psvrecore_host_default OFF)
else()
    set(_psvrecore_host_default ON)
endif()
option(PSVRECORE_BUILD_TOOLS "Build host-side tools" ${_psvrecore_host_default})
option(PSVRECORE_BUILD_TESTS "Build unit tests (no game data or SDK files required)" ${_psvrecore_host_default})
option(PSVRECORE_PROFILER "Build Tracy into nx-foundation and compile its profiler zones" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(PsvrecoreCompilerOptions)

# nx-foundation (github.com/AdamFull/nx-foundation, submodule in libs/nx-foundation), unless the including project
# provides it. Its own third-party code is in submodules too (git submodule update --init --recursive).
if(NOT TARGET nx::foundation)
    set(NX_FOUNDATION_TESTS OFF CACHE BOOL "" FORCE)
    set(NX_FOUNDATION_TRACY ${PSVRECORE_PROFILER} CACHE BOOL "" FORCE)
    set(NX_ENABLE_PROFILER ${PSVRECORE_PROFILER} CACHE BOOL "" FORCE)
    # Games are shared libraries on Android (libmain.so, loaded by the Java activity with dlopen), where mimalloc's
    # default initial-exec thread-local storage fails to load.
    if(ANDROID)
        set(MI_LOCAL_DYNAMIC_TLS ON CACHE BOOL "" FORCE)
    endif()
    add_subdirectory(libs/nx-foundation EXCLUDE_FROM_ALL)
endif()

add_subdirectory(libs/core)
add_subdirectory(libs/formats)
add_subdirectory(libs/arm)
add_subdirectory(libs/analysis)
add_subdirectory(libs/recomp)
add_subdirectory(libs/runtime)
add_subdirectory(libs/usse)
add_subdirectory(libs/vulkan)
add_subdirectory(libs/gxm)
add_subdirectory(libs/platform)
add_subdirectory(libs/frontend)
if(PSVRECORE_BUILD_TESTS)
    add_subdirectory(libs/interp)  # (test-only)
endif()

# vita-headers (github.com/vitasdk/vita-headers, MIT, submodule in third_party/vita-headers): its NID database names
# the system libraries' functions. Tools load it at run time from here unless told otherwise.
set(PSVRECORE_NID_DB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vita-headers/db/360" CACHE PATH
    "NID database the tools load by default")

if(PSVRECORE_BUILD_TOOLS)
    add_subdirectory(tools/psvinfo)
    add_subdirectory(tools/psvdis)
    add_subdirectory(tools/psvrecomp)
    add_subdirectory(tools/psvusse)
    add_subdirectory(tools/psvrender)
endif()

if(PSVRECORE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
