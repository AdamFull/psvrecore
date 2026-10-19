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
psvrecore_target_defaults(psvrecore_runtime)
