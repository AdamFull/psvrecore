# The runtime recompiled programs run on: guest memory, the kernel (threads, scheduling, synchronisation), files, the
# module loader, and the system libraries implemented on the host (HLE).
add_library(psvrecore_runtime STATIC
    src/filesystem.cpp
    src/hle.cpp
    src/input.cpp
    src/hle/apputil.cpp
    src/hle/audioout.cpp
    src/hle/ctrl.cpp
    src/hle/fios2.cpp
    src/hle/iofilemgr.cpp
    src/hle/libkernel.cpp
    src/hle/modulemgr.cpp
    src/hle/ngs.cpp
    src/hle/processmgr.cpp
    src/hle/rtc.cpp
    src/hle/sysmem.cpp
    src/hle/threadmgr.cpp
    src/hle/touch.cpp
    src/kernel/scheduler.cpp
    src/kernel/sync.cpp
    src/kernel/threads.cpp
    src/loader.cpp
    src/memory.cpp
    src/ngs/modules.cpp
    src/runtime.cpp
    src/savedata.cpp
)
if(WIN32)
    target_sources(psvrecore_runtime PRIVATE src/host_win32.cpp)
else()
    target_sources(psvrecore_runtime PRIVATE src/host_posix.cpp)
endif()
add_library(psvrecore::runtime ALIAS psvrecore_runtime)
target_include_directories(psvrecore_runtime PUBLIC include)
target_link_libraries(psvrecore_runtime PUBLIC psvrecore::recomp_support psvrecore::analysis)

# LibAtrac9 (github.com/Thealexbarney/LibAtrac9, MIT, submodule in third_party/LibAtrac9): the ATRAC9 decoder of
# NGS's ATRAC9 player.
file(GLOB PSVRECORE_LIBATRAC9_SOURCES CONFIGURE_DEPENDS "${PSVRECORE_DIR}/third_party/LibAtrac9/C/src/*.c")
add_library(psvrecore_libatrac9 STATIC ${PSVRECORE_LIBATRAC9_SOURCES})
target_include_directories(psvrecore_libatrac9 SYSTEM PUBLIC "${PSVRECORE_DIR}/third_party/LibAtrac9/C/src")
# (its header declares the functions for a DLL under MSVC-style compilers; as a static library they are defined here)
target_compile_definitions(psvrecore_libatrac9 PUBLIC COMPILING_DLL)
target_link_libraries(psvrecore_runtime PRIVATE psvrecore_libatrac9)

# openh264 (github.com/cisco/openh264, BSD-2-Clause, submodule in third_party/openh264): AvPlayer's H.264 decoder,
# its C++ only (no assembly).
set(PSVRECORE_OPENH264 "${PSVRECORE_DIR}/third_party/openh264/codec")
file(GLOB PSVRECORE_OPENH264_SOURCES CONFIGURE_DEPENDS
    "${PSVRECORE_OPENH264}/common/src/*.cpp"
    "${PSVRECORE_OPENH264}/decoder/core/src/*.cpp")
add_library(psvrecore_openh264 STATIC ${PSVRECORE_OPENH264_SOURCES} "${PSVRECORE_OPENH264}/decoder/plus/src/welsDecoderExt.cpp")
target_include_directories(psvrecore_openh264 SYSTEM PUBLIC "${PSVRECORE_OPENH264}/api/wels")
target_include_directories(psvrecore_openh264 PRIVATE
    "${PSVRECORE_OPENH264}/api"
    "${PSVRECORE_OPENH264}/common/inc"
    "${PSVRECORE_OPENH264}/decoder/core/inc"
    "${PSVRECORE_OPENH264}/decoder/plus/inc")
target_link_libraries(psvrecore_runtime PRIVATE psvrecore_openh264)

# The PacketVideo AAC decoder (Android Open Source Project, Apache-2.0, vendored in third_party/pvaacdec): AvPlayer's
# AAC decoder.
file(GLOB PSVRECORE_PVAACDEC_SOURCES CONFIGURE_DEPENDS "${PSVRECORE_DIR}/third_party/pvaacdec/src/*.cpp")
add_library(psvrecore_pvaacdec STATIC ${PSVRECORE_PVAACDEC_SOURCES})
target_include_directories(psvrecore_pvaacdec SYSTEM PUBLIC "${PSVRECORE_DIR}/third_party/pvaacdec/src")
# (its tables narrow constants into Int32 in braces, written before C++11)
target_compile_options(psvrecore_pvaacdec PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wno-c++11-narrowing -Wno-narrowing>)
# (what Android's build of it defined: AAC+ with high-quality SBR and parametric stereo, no OSCL linkage macros)
target_compile_definitions(psvrecore_pvaacdec PRIVATE AAC_PLUS HQ_SBR PARAMETRICSTEREO OSCL_EXPORT_REF= OSCL_UNUSED_ARG=)
target_compile_definitions(psvrecore_pvaacdec PUBLIC OSCL_IMPORT_REF=)
target_link_libraries(psvrecore_runtime PRIVATE psvrecore_pvaacdec)
psvrecore_target_defaults(psvrecore_runtime)
