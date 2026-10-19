# The runtime recompiled programs run on: guest memory, the kernel (threads, scheduling, synchronisation), files, the
# module loader, and the system libraries implemented on the host (HLE).
add_library(psvrecore_runtime STATIC
    src/filesystem.cpp
    src/hle.cpp
    src/input.cpp
    src/hle/audioout.cpp
    src/hle/ctrl.cpp
    src/hle/fios2.cpp
    src/hle/iofilemgr.cpp
    src/hle/libkernel.cpp
    src/hle/modulemgr.cpp
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
    src/runtime.cpp
)
if(WIN32)
    target_sources(psvrecore_runtime PRIVATE src/host_win32.cpp)
else()
    target_sources(psvrecore_runtime PRIVATE src/host_posix.cpp)
endif()
add_library(psvrecore::runtime ALIAS psvrecore_runtime)
target_include_directories(psvrecore_runtime PUBLIC include)
target_link_libraries(psvrecore_runtime PUBLIC psvrecore::recomp_support psvrecore::analysis)
psvrecore_target_defaults(psvrecore_runtime)
