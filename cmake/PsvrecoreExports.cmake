# cmake -DOBJECTS=<list file> -DOUTPUT=<.def> -DEXTRA=<names> -P PsvrecoreExports.cmake
#
# Writes the module-definition file of psvrecore.dll's nx-foundation exports: the external symbols of the objects
# listed in OBJECTS (one path a line), as CMake's WINDOWS_EXPORT_ALL_SYMBOLS lists them, narrowed to nx-foundation's
# own functions (C++ names in namespace nx) and mimalloc's and nx-foundation's C functions (mi_*, nx_*); then the
# names in EXTRA. Left out: data (a program reaches the library's data only through dllimport, which nx-foundation's
# headers do not declare), run-time type information, and the C runtime's and the C++ library's inline functions
# the objects happen to carry (a program compiles its own). The functions marked PSVRECORE_API export themselves.

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E __create_def "${OUTPUT}.tmp" "${OBJECTS}"
    RESULT_VARIABLE _result)
if(_result)
    message(FATAL_ERROR "listing the exports of ${OBJECTS} failed (${_result})")
endif()
file(STRINGS "${OUTPUT}.tmp" _lines)
file(REMOVE "${OUTPUT}.tmp")
set(_def "EXPORTS\n")
foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "" OR _line STREQUAL "EXPORTS" OR _line MATCHES "[ \t]DATA$")
        continue()
    endif()
    if(_line MATCHES "^\\?")
        if(_line MATCHES "^\\?\\?_R" OR NOT _line MATCHES "@nx@@")
            continue()
        endif()
    elseif(NOT _line MATCHES "^(mi|nx)_")
        continue()
    endif()
    string(APPEND _def "\t${_line}\n")
endforeach()
foreach(_name IN LISTS EXTRA)
    string(APPEND _def "\t${_name}\n")
endforeach()
file(WRITE "${OUTPUT}" "${_def}")
