# The SDK's GXM samples (scripts/sample_check.py): added when PSVRECORE_SAMPLES_DIR names a folder the script filled
# with SDK-built samples and their recompiled code. Nothing here is built otherwise, and nothing it builds is
# committed.

set(_sample_src "${CMAKE_CURRENT_LIST_DIR}")

function(psvrecore_sample_libc)
    if(TARGET psvrecore_sample_libc)
        return()
    endif()
    file(GLOB _libc_sources "${PSVRECORE_SAMPLE_LIBC_GENERATED}/*.c")
    add_library(psvrecore_sample_libc STATIC ${_libc_sources})
    target_include_directories(psvrecore_sample_libc PRIVATE "${PSVRECORE_SAMPLE_LIBC_GENERATED}")
    target_link_libraries(psvrecore_sample_libc PUBLIC psvrecore::recomp_support)
    psvrecore_generated_code(psvrecore_sample_libc)
    file(GLOB _fios2_sources "${PSVRECORE_SAMPLE_FIOS2_GENERATED}/*.c")
    add_library(psvrecore_sample_libfios2 STATIC ${_fios2_sources})
    target_include_directories(psvrecore_sample_libfios2 PRIVATE "${PSVRECORE_SAMPLE_FIOS2_GENERATED}")
    target_link_libraries(psvrecore_sample_libfios2 PUBLIC psvrecore::recomp_support)
    psvrecore_generated_code(psvrecore_sample_libfios2)
endfunction()

# NAME: the sample; GENERATED: its recompiled code (module name sample_<NAME>).
function(psvrecore_add_sample)
    cmake_parse_arguments(P "" "NAME;GENERATED" "" ${ARGN})
    psvrecore_sample_libc()
    file(GLOB _gen "${P_GENERATED}/*.c")
    add_library(psvrecore_sample_${P_NAME}_code STATIC ${_gen})
    target_include_directories(psvrecore_sample_${P_NAME}_code PRIVATE "${P_GENERATED}")
    target_link_libraries(psvrecore_sample_${P_NAME}_code PUBLIC psvrecore::recomp_support)
    psvrecore_generated_code(psvrecore_sample_${P_NAME}_code)

    add_executable(sample_${P_NAME} "${_sample_src}/runner.cpp" "${_sample_src}/png.cpp")
    target_compile_definitions(sample_${P_NAME} PRIVATE PSVRECORE_SAMPLE_MODULE=sample_${P_NAME}_module)
    target_link_libraries(sample_${P_NAME} PRIVATE psvrecore::runtime psvrecore::gxm_hle psvrecore::platform psvrecore_sample_${P_NAME}_code
                                                  psvrecore_sample_libc psvrecore_sample_libfios2 nx::foundation)
    psvrecore_target_defaults(sample_${P_NAME})
endfunction()

include("${PSVRECORE_SAMPLES_DIR}/samples.cmake")
