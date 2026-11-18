# The runtime recompiled programs run on: guest memory, the kernel (threads, scheduling, synchronisation), files, the
# module loader, and the system libraries implemented on the host (HLE).
add_library(psvrecore_runtime STATIC
    src/filesystem.cpp
    src/hle.cpp
    src/input.cpp
    src/hle/apputil.cpp
    src/hle/commondialog.cpp
    src/hle/avplayer.cpp
    src/hle/audioout.cpp
    src/hle/audiodec.cpp
    src/hle/codecengine.cpp
    src/hle/ctrl.cpp
    src/hle/debug.cpp
    src/hle/fiber.cpp
    src/hle/fios2.cpp
    src/hle/ime.cpp
    src/hle/iofilemgr.cpp
    src/hle/jpegenc.cpp
    src/hle/json.cpp
    src/hle/kernel_more.cpp
    src/hle/libkernel.cpp
    src/hle/modulemgr.cpp
    src/hle/mp4lib.cpp
    src/hle/motion.cpp
    src/hle/mspace.cpp
    src/hle/net.cpp
    src/hle/host_socket.cpp
    src/hle/ngs.cpp
    src/hle/np.cpp
    src/hle/online.cpp
    src/hle/processmgr.cpp
    src/hle/psn.cpp
    src/hle/pvf.cpp
    src/hle/rtc.cpp
    src/hle/sas.cpp
    src/hle/services.cpp
    src/hle/sysmem.cpp
    src/hle/system.cpp
    src/hle/systemgesture.cpp
    src/hle/threadmgr.cpp
    src/hle/touch.cpp
    src/hle/videodec.cpp
    src/hle/voice.cpp
    src/kernel/backend_fibers.cpp
    src/kernel/backend_native.cpp
    src/kernel/scheduler.cpp
    src/kernel/sync.cpp
    src/kernel/threads.cpp
    src/kernel/objects.cpp
    src/loader.cpp
    src/memory.cpp
    src/media/decoders.cpp
    src/media/mp4.cpp
    src/media/jpeg.cpp
    src/media/font.cpp
    src/media/mp3.cpp
    src/ngs/modules.cpp
    src/runtime.cpp
    src/savedata.cpp
)
if(WIN32)
    target_sources(psvrecore_runtime PRIVATE src/host_win32.cpp)
    target_link_libraries(psvrecore_runtime PRIVATE dbghelp ws2_32 iphlpapi)
else()
    target_sources(psvrecore_runtime PRIVATE src/host_posix.cpp)
endif()
add_library(psvrecore::runtime ALIAS psvrecore_runtime)
if(PSVRECORE_GUEST_FIBERS)
    target_compile_definitions(psvrecore_runtime PUBLIC PSVRECORE_GUEST_FIBERS=1)
endif()
target_include_directories(psvrecore_runtime PUBLIC include)
target_link_libraries(psvrecore_runtime PUBLIC psvrecore::recomp_support psvrecore::analysis)

# LibAtrac9 (github.com/Thealexbarney/LibAtrac9, MIT, submodule in third_party/LibAtrac9): the ATRAC9 decoder of
# NGS's ATRAC9 player.
file(GLOB PSVRECORE_LIBATRAC9_SOURCES CONFIGURE_DEPENDS "${PSVRECORE_DIR}/third_party/LibAtrac9/C/src/*.c")
# (its API through a wrapper that leaves out the header's DLL export marks: internal to the runtime)
list(FILTER PSVRECORE_LIBATRAC9_SOURCES EXCLUDE REGEX "/libatrac9\.c$")
add_library(psvrecore_libatrac9 STATIC ${PSVRECORE_LIBATRAC9_SOURCES} src/media/atrac9_api.c)
target_include_directories(psvrecore_libatrac9 SYSTEM PUBLIC "${PSVRECORE_DIR}/third_party/LibAtrac9/C/src")
# (its header declares the functions for a DLL under MSVC-style compilers; as a static library they are defined here)
target_compile_definitions(psvrecore_libatrac9 PUBLIC COMPILING_DLL)
target_link_libraries(psvrecore_runtime PRIVATE psvrecore_libatrac9)

# minimp3 (github.com/lieff/minimp3 at 9f1f7c67d596, CC0, the header and licence copied into third_party/minimp3):
# SceAudiodecUser's MP3 decoder (src/media/mp3.cpp holds its implementation).
target_include_directories(psvrecore_runtime SYSTEM PRIVATE "${PSVRECORE_DIR}/third_party/minimp3")

# ScePvf's system fonts, built in as byte arrays (SIL OFL 1.1): Roboto Regular and Bold (third_party/roboto) and Noto
# Sans JP, KR, SC and TC Regular (third_party/noto-cjk). Each becomes generated/fonts/<name>.cpp, written again only
# when its font is newer (turning a Noto font of several MB into text takes a few seconds).
set(_psvrecore_fonts
    "roboto/Roboto-Regular.ttf" "roboto/Roboto-Bold.ttf"
    "noto-cjk/NotoSansJP-Regular.otf" "noto-cjk/NotoSansKR-Regular.otf"
    "noto-cjk/NotoSansSC-Regular.otf" "noto-cjk/NotoSansTC-Regular.otf")
foreach(_font IN LISTS _psvrecore_fonts)
    set(_path "${PSVRECORE_DIR}/third_party/${_font}")
    get_filename_component(_name "${_font}" NAME_WE)
    string(REPLACE "-" "_" _symbol "${_name}")
    set(_cpp "${CMAKE_CURRENT_BINARY_DIR}/generated/fonts/${_name}.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_path}")
    if(NOT EXISTS "${_cpp}" OR "${_path}" IS_NEWER_THAN "${_cpp}")
        file(READ "${_path}" _hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
        file(WRITE "${_cpp}" "// Generated from third_party/${_font} by libs/runtime/CMakeLists.txt.
#include <cstddef>
#include <cstdint>
namespace psvrecore::runtime::hle {
extern const std::uint8_t k${_symbol}[] = {${_bytes}};
extern const std::size_t k${_symbol}Size = sizeof k${_symbol};
}  // namespace psvrecore::runtime::hle
")
    endif()
    target_sources(psvrecore_runtime PRIVATE "${_cpp}")
endforeach()

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
# (its tables narrow constants into Int32 in braces, written before C++11; its warnings are its own, unchanged code)
target_compile_options(psvrecore_pvaacdec PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-Wno-c++11-narrowing -Wno-narrowing -w>)
# (what Android's build of it defined: AAC+ with high-quality SBR and parametric stereo, no OSCL linkage macros)
target_compile_definitions(psvrecore_pvaacdec PRIVATE AAC_PLUS HQ_SBR PARAMETRICSTEREO OSCL_EXPORT_REF= OSCL_UNUSED_ARG=)
target_compile_definitions(psvrecore_pvaacdec PUBLIC OSCL_IMPORT_REF=)
target_link_libraries(psvrecore_runtime PRIVATE psvrecore_pvaacdec)
psvrecore_target_defaults(psvrecore_runtime)
