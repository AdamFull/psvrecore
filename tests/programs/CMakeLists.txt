# Test programs (scripts/program_check.py): added when PSVRECORE_PROGRAMS_DIR names a folder the script filled with
# SDK-built programs and their recompiled code. Nothing here is built otherwise, and nothing it builds is committed.
#
# ${PSVRECORE_PROGRAMS_DIR}/programs.cmake sets PSVRECORE_PROGRAM_LIBC_GENERATED and PSVRECORE_PROGRAM_FIOS2_GENERATED,
# then calls psvrecore_add_program() for each program.

set(_program_src "${CMAKE_CURRENT_LIST_DIR}")

function(psvrecore_program_libc)
    if(TARGET psvrecore_program_libc)
        return()
    endif()
    file(GLOB _libc_sources "${PSVRECORE_PROGRAM_LIBC_GENERATED}/*.c")
    add_library(psvrecore_program_libc STATIC ${_libc_sources})
    target_include_directories(psvrecore_program_libc PRIVATE "${PSVRECORE_PROGRAM_LIBC_GENERATED}")
    target_link_libraries(psvrecore_program_libc PUBLIC psvrecore::recomp_support)
    psvrecore_generated_code(psvrecore_program_libc)
    file(GLOB _fios2_sources "${PSVRECORE_PROGRAM_FIOS2_GENERATED}/*.c")
    add_library(psvrecore_program_libfios2 STATIC ${_fios2_sources})
    target_include_directories(psvrecore_program_libfios2 PRIVATE "${PSVRECORE_PROGRAM_FIOS2_GENERATED}")
    target_link_libraries(psvrecore_program_libfios2 PUBLIC psvrecore::recomp_support)
    psvrecore_generated_code(psvrecore_program_libfios2)
endfunction()

# NAME: the program; GENERATED: its recompiled code (module name program_<NAME>).
function(psvrecore_add_program)
    cmake_parse_arguments(P "" "NAME;GENERATED" "" ${ARGN})
    psvrecore_program_libc()
    file(GLOB _gen "${P_GENERATED}/*.c")
    add_library(psvrecore_program_${P_NAME}_code STATIC ${_gen})
    target_include_directories(psvrecore_program_${P_NAME}_code PRIVATE "${P_GENERATED}")
    target_link_libraries(psvrecore_program_${P_NAME}_code PUBLIC psvrecore::recomp_support)
    psvrecore_generated_code(psvrecore_program_${P_NAME}_code)

    add_executable(program_${P_NAME} "${_program_src}/runner.cpp")
    target_compile_definitions(program_${P_NAME} PRIVATE PSVRECORE_PROGRAM_MODULE=program_${P_NAME}_module)
    target_link_libraries(program_${P_NAME} PRIVATE psvrecore::runtime psvrecore_program_${P_NAME}_code psvrecore_program_libc
                                                   psvrecore_program_libfios2 nx::foundation)
    psvrecore_target_defaults(program_${P_NAME})
endfunction()

include("${PSVRECORE_PROGRAMS_DIR}/programs.cmake")
