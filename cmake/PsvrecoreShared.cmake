# What a game links: psvrecore::game for its executable, psvrecore::generated_code for its recompiled modules. With
# PSVRECORE_SHARED, the shared runtime library (psvrecore.dll, libpsvrecore.so.<ABI version>); without, the static
# libraries (the frontend; the function registry and trap handling, recomp_support).
#
# The shared library is made from the objects of psvrecore's runtime libraries (runtime, kernel and system libraries,
# gxm, vulkan, platform, frontend and what they build on), with nx-foundation and its allocator (mimalloc) inside it
# too: the game's executable uses nx-foundation's strings, containers and log through psvrecore's interface, and
# there must be one heap and one log in the process. The third-party libraries (SDL3, openh264, LibAtrac9, the AAC
# decoder, volk, VMA, zlib) are linked in as they are and stay internal.
#
# Exported: what abi.h's PSVRECORE_API marks, nx-foundation's and mimalloc's functions, and SDL_RunApp (the main()
# that SDL_main.h gives a game's executable on Windows calls it). On Linux everything else is hidden
# (-fvisibility=hidden for psvrecore's code, --exclude-libs for the archives linked in).

# The ABI version, from abi.h (also the SOVERSION).
file(STRINGS "${PSVRECORE_DIR}/libs/core/include/psvrecore/abi.h" _psvrecore_abi_line
     REGEX "^#define PSVRECORE_ABI_VERSION [0-9]+$")
string(REGEX REPLACE "^#define PSVRECORE_ABI_VERSION ([0-9]+)$" "\\1" PSVRECORE_ABI_VERSION "${_psvrecore_abi_line}")
if(NOT PSVRECORE_ABI_VERSION)
    message(FATAL_ERROR "PSVRECORE_ABI_VERSION not found in abi.h")
endif()

add_library(psvrecore_game INTERFACE)
add_library(psvrecore::game ALIAS psvrecore_game)
add_library(psvrecore_generated_code INTERFACE)
add_library(psvrecore::generated_code ALIAS psvrecore_generated_code)

if(NOT PSVRECORE_SHARED)
    target_link_libraries(psvrecore_game INTERFACE psvrecore::frontend)
    target_link_libraries(psvrecore_generated_code INTERFACE psvrecore::recomp_support)
    return()
endif()

# The libraries whose objects make up the shared library, in dependency order.
set(PSVRECORE_SHARED_COMPONENTS
    psvrecore_core psvrecore_formats psvrecore_arm psvrecore_analysis psvrecore_recomp_support psvrecore_runtime
    psvrecore_usse psvrecore_vulkan psvrecore_gxm psvrecore_gxm_hle psvrecore_platform psvrecore_frontend)
# (nx-foundation and its allocator, and Tracy when the profiler is built in: one copy of their state in the process)
set(_psvrecore_foundation nx_foundation)
if(TARGET mimalloc-static)
    list(APPEND _psvrecore_foundation mimalloc-static)
    if(NOT WIN32)
        # (mimalloc's own build hides its functions; the game's executable allocates through them)
        target_compile_definitions(mimalloc-static PRIVATE MI_SHARED_LIB MI_SHARED_LIB_EXPORT)
    endif()
endif()
if(PSVRECORE_TRACY AND TARGET TracyClient)
    list(APPEND _psvrecore_foundation TracyClient)
endif()

foreach(_component IN LISTS PSVRECORE_SHARED_COMPONENTS)
    # (PSVRECORE_API is dllexport / default visibility in psvrecore's own code; the tests and tools that link these
    # libraries statically see it empty)
    target_compile_definitions(${_component} PRIVATE PSVRECORE_SHARED PSVRECORE_BUILDING)
    set_target_properties(${_component} PROPERTIES
        C_VISIBILITY_PRESET hidden CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
endforeach()

set(_psvrecore_objects "")
foreach(_target IN LISTS PSVRECORE_SHARED_COMPONENTS _psvrecore_foundation)
    list(APPEND _psvrecore_objects "$<TARGET_OBJECTS:${_target}>")
endforeach()
add_library(psvrecore_shared SHARED ${_psvrecore_objects})
add_library(psvrecore::shared ALIAS psvrecore_shared)
# (the static libraries too, for what they link: the third-party archives, system libraries, nx-foundation's
# assembly; their own objects are already in)
target_link_libraries(psvrecore_shared PRIVATE ${PSVRECORE_SHARED_COMPONENTS} ${_psvrecore_foundation})
set_target_properties(psvrecore_shared PROPERTIES
    OUTPUT_NAME psvrecore
    # (next to the game's executable: the top of the build folder)
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    SOVERSION ${PSVRECORE_ABI_VERSION}
    VERSION ${PSVRECORE_ABI_VERSION})
if(WIN32)
    # nx-foundation's and mimalloc's functions, listed from their objects by CMake's own .def writer.
    set(_psvrecore_def_objects "${CMAKE_CURRENT_BINARY_DIR}/psvrecore_exports.objects")
    set(_psvrecore_def "${CMAKE_CURRENT_BINARY_DIR}/psvrecore_exports.def")
    set(_psvrecore_def_list "")
    foreach(_target IN LISTS _psvrecore_foundation)
        list(APPEND _psvrecore_def_list "$<TARGET_OBJECTS:${_target}>")
    endforeach()
    file(GENERATE OUTPUT "${_psvrecore_def_objects}" CONTENT "$<JOIN:${_psvrecore_def_list},\n>\n")
    add_custom_command(
        OUTPUT "${_psvrecore_def}"
        COMMAND "${CMAKE_COMMAND}" "-DOBJECTS=${_psvrecore_def_objects}" "-DOUTPUT=${_psvrecore_def}"
                "-DEXTRA=SDL_RunApp" -P "${PSVRECORE_DIR}/cmake/PsvrecoreExports.cmake"
        DEPENDS ${_psvrecore_foundation} "${_psvrecore_def_objects}" "${PSVRECORE_DIR}/cmake/PsvrecoreExports.cmake"
        COMMENT "Listing nx-foundation's exports from psvrecore"
        VERBATIM)
    target_sources(psvrecore_shared PRIVATE "${_psvrecore_def}")
elseif(NOT APPLE)
    target_link_options(psvrecore_shared PRIVATE "LINKER:--exclude-libs,ALL")
endif()

# A game: the library, and the headers as psvrecore was compiled with them (without linking the static libraries).
target_link_libraries(psvrecore_game INTERFACE psvrecore_shared "$<COMPILE_ONLY:psvrecore_frontend>")
target_compile_definitions(psvrecore_game INTERFACE PSVRECORE_SHARED)
if(WIN32)
    # (debug builds: nx-foundation's allocation guard reads a thread-local variable of the library, which an executable
    # cannot reach in a DLL; the game's own allocations go unguarded, the library's are still checked)
    target_compile_definitions(psvrecore_game INTERFACE NX_ALLOC_GUARD=0)
endif()
target_link_libraries(psvrecore_generated_code INTERFACE psvrecore_shared "$<COMPILE_ONLY:psvrecore_recomp_support>")
target_compile_definitions(psvrecore_generated_code INTERFACE PSVRECORE_SHARED)

message(STATUS "psvrecore: shared runtime library (ABI version ${PSVRECORE_ABI_VERSION})")
