# Same-source probes (scripts/probe_check.py): added when PSVRECORE_PROBES_DIR names a folder the script filled with
# SDK-built probes and their recompiled code. Nothing here is built otherwise, and nothing it builds is committed.
#
# ${PSVRECORE_PROBES_DIR}/probes.cmake sets PSVRECORE_PROBE_LIBC_SELF, PSVRECORE_PROBE_LIBC_BASES and
# PSVRECORE_PROBE_LIBC_GENERATED, then calls psvrecore_add_probe() for each probe.

set(_probe_src "${CMAKE_CURRENT_LIST_DIR}")

function(psvrecore_probe_libc)
    if(TARGET psvrecore_probe_libc)
        return()
    endif()
    file(GLOB _libc_sources "${PSVRECORE_PROBE_LIBC_GENERATED}/*.c")
    add_library(psvrecore_probe_libc STATIC ${_libc_sources})
    target_include_directories(psvrecore_probe_libc PRIVATE "${PSVRECORE_PROBE_LIBC_GENERATED}")
    target_link_libraries(psvrecore_probe_libc PUBLIC psvrecore::recomp_support)
    psvrecore_generated_code(psvrecore_probe_libc)
endfunction()

# NAME: the probe; SOURCE: its C file; GENERATED: its recompiled code (module name probe_<NAME>); SELF and BASES: the
# module file and the addresses it was generated for; ENTRY: probe_main's code address.
function(psvrecore_add_probe)
    cmake_parse_arguments(P "" "NAME;SOURCE;GENERATED;SELF;BASES;ENTRY" "" ${ARGN})
    psvrecore_probe_libc()
    file(GLOB _gen "${P_GENERATED}/*.c")
    add_library(psvrecore_probe_${P_NAME}_code STATIC ${_gen})
    target_include_directories(psvrecore_probe_${P_NAME}_code PRIVATE "${P_GENERATED}")
    target_link_libraries(psvrecore_probe_${P_NAME}_code PUBLIC psvrecore::recomp_support)
    psvrecore_generated_code(psvrecore_probe_${P_NAME}_code)

    # The same probe for the host: probe_main renamed.
    add_library(psvrecore_probe_${P_NAME}_host STATIC "${P_SOURCE}")
    target_compile_definitions(psvrecore_probe_${P_NAME}_host PRIVATE probe_main=host_probe_main)
    target_include_directories(psvrecore_probe_${P_NAME}_host PRIVATE "${_probe_src}")
    psvrecore_generated_code(psvrecore_probe_${P_NAME}_host)  # (the same floating-point and integer semantics)

    add_executable(probe_${P_NAME} "${_probe_src}/runner.cpp")
    target_compile_definitions(probe_${P_NAME} PRIVATE
        PSVRECORE_PROBE_NAME="${P_NAME}"
        PSVRECORE_PROBE_MODULE=probe_${P_NAME}_module
        PSVRECORE_PROBE_SELF="${P_SELF}"
        PSVRECORE_PROBE_BASES="${P_BASES}"
        PSVRECORE_PROBE_ENTRY=${P_ENTRY}u
        PSVRECORE_LIBC_SELF="${PSVRECORE_PROBE_LIBC_SELF}"
        PSVRECORE_LIBC_BASES="${PSVRECORE_PROBE_LIBC_BASES}")
    target_link_libraries(probe_${P_NAME} PRIVATE psvrecore::analysis psvrecore::recomp_support psvrecore_probe_${P_NAME}_code
                                                 psvrecore_probe_${P_NAME}_host psvrecore_probe_libc nx::foundation)
    psvrecore_target_defaults(probe_${P_NAME})
endfunction()

include("${PSVRECORE_PROBES_DIR}/probes.cmake")
