# Brings psvrecore into a game's build. A game's CMakeLists.txt, before its targets:
#
#   if(DEFINED ENV{PSVRECORE_ROOT})
#       set(PSVRECORE_ROOT "$ENV{PSVRECORE_ROOT}" CACHE PATH "psvrecore's checkout")
#   else()
#       set(PSVRECORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../psvrecore" CACHE PATH "psvrecore's checkout")
#   endif()
#   include("${PSVRECORE_ROOT}/cmake/UsePsvrecore.cmake")
#
# By default the game builds against psvrecore's installed package for the same preset,
# <PSVRECORE_ROOT>/install/<PSVRECORE_PRESET> (scripts\build.ps1 -Preset <preset> -NoTest -Install in psvrecore; the
# game's presets set PSVRECORE_PRESET to their own name). psvrecore_DIR, when set, names another package.
# PSVRECORE_FROM_SOURCE=ON builds psvrecore's sources with the game instead, for changes to both at once.
#
# <game>/psvrecore.version holds the psvrecore commit the game was last checked with; a package from another commit
# gets a warning (the game may need its code generated again).

include_guard(GLOBAL)

option(PSVRECORE_FROM_SOURCE "Build psvrecore from PSVRECORE_ROOT's sources with the game (changes to both at once)" OFF)
set(PSVRECORE_PRESET "" CACHE STRING "The preset of psvrecore's package the game builds against (install/<preset>)")

if(NOT EXISTS "${PSVRECORE_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "PSVRECORE_ROOT (${PSVRECORE_ROOT}) is not a psvrecore checkout: set PSVRECORE_ROOT")
endif()

if(PSVRECORE_FROM_SOURCE)
    add_subdirectory("${PSVRECORE_ROOT}" psvrecore)
    execute_process(COMMAND git rev-parse HEAD WORKING_DIRECTORY "${PSVRECORE_ROOT}"
        OUTPUT_VARIABLE PSVRECORE_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
else()
    # (the options of a build from source, which the game's presets may set: read, so CMake does not call them unused)
    set(_psvrecore_source_options "${PSVRECORE_BUILD_TESTS}${PSVRECORE_BUILD_TOOLS}")
    if(NOT psvrecore_DIR)
        if(NOT PSVRECORE_PRESET)
            message(FATAL_ERROR "PSVRECORE_PRESET is not set: the game's presets set it (\"PSVRECORE_PRESET\": "
                "\"\${presetName}\"), or give psvrecore_DIR")
        endif()
        set(_psvrecore_package "${PSVRECORE_ROOT}/install/${PSVRECORE_PRESET}")
        if(NOT EXISTS "${_psvrecore_package}/lib/cmake/psvrecore/psvrecoreConfig.cmake")
            message(FATAL_ERROR "psvrecore is not installed for ${PSVRECORE_PRESET} (${_psvrecore_package}): in "
                "${PSVRECORE_ROOT}, scripts\\build.ps1 -Preset ${PSVRECORE_PRESET} -NoTest -Install")
        endif()
        set(psvrecore_DIR "${_psvrecore_package}/lib/cmake/psvrecore" CACHE PATH "psvrecore's package" FORCE)
    endif()
    # (NO_CMAKE_FIND_ROOT_PATH: the NDK's toolchain would look for packages in its sysroot only)
    find_package(psvrecore CONFIG REQUIRED NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
endif()

set(_psvrecore_version_file "${CMAKE_SOURCE_DIR}/psvrecore.version")
set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_psvrecore_version_file}")
if(EXISTS "${_psvrecore_version_file}")
    file(STRINGS "${_psvrecore_version_file}" _psvrecore_checked REGEX "^[0-9a-f]+" LIMIT_COUNT 1)
    string(REGEX MATCH "^[0-9a-f]+" _psvrecore_checked "${_psvrecore_checked}")
    if(_psvrecore_checked AND PSVRECORE_GIT_COMMIT)
        string(LENGTH "${_psvrecore_checked}" _len)
        string(SUBSTRING "${PSVRECORE_GIT_COMMIT}" 0 ${_len} _psvrecore_used)
        if(NOT _psvrecore_used STREQUAL _psvrecore_checked)
            message(WARNING "psvrecore ${PSVRECORE_GIT_COMMIT} is not the commit this game was last checked with "
                "(${_psvrecore_checked}, psvrecore.version): its code may need generating again "
                "(scripts/generate_code.py), and psvrecore.version updating once it runs")
        endif()
    endif()
endif()
