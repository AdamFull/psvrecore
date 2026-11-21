# psvrecore as a package that games build against without building it (cmake --install of a preset's build folder,
# then find_package(psvrecore CONFIG) in the game with CMAKE_PREFIX_PATH or psvrecore_DIR pointing at it):
#
#   <prefix>/lib/cmake/psvrecore/  psvrecoreConfig.cmake and the imported targets
#   <prefix>/bin, <prefix>/lib     the runtime library (psvrecore.dll / libpsvrecore.so.<ABI version>, or on Android
#                                  the static libraries), and the host tools (psvrecomp, psvinfo, ...)
#   <prefix>/include/src/...       the headers a game compiles with, each include folder at its path in this checkout
#   <prefix>/include/build/...     (and those generated in the build folder)
#   <prefix>/share/psvrecore/cmake the functions a game calls (psvrecore_target_defaults, psvrecore_generated_code,
#                                  psvrecore_game_package)
#
# The package has what psvrecore::game and psvrecore::generated_code have here: each library they reach (psvrecore's
# own, nx-foundation's, the third-party ones) becomes an imported target with the same usage requirements, its include
# folders moved into the package. CMake's install(EXPORT) cannot do this without every third-party project's targets
# being installable in psvrecore's export set; reading the usage requirements here leaves their CMake files as they are.
# What a target's requirements hold that this does not know (a generator expression other than BUILD_INTERFACE,
# INSTALL_INTERFACE, LINK_ONLY, COMPILE_ONLY and TARGET_NAME in a list of libraries or folders, a path of this checkout
# in another property) stops the configure step: the package would be wrong otherwise.

include(CMakePackageConfigHelpers)

set(_psvrecore_pkg_dir "${CMAKE_BINARY_DIR}/psvrecore-package")
cmake_path(NORMAL_PATH PSVRECORE_DIR OUTPUT_VARIABLE _psvrecore_pkg_src)
cmake_path(NORMAL_PATH CMAKE_BINARY_DIR OUTPUT_VARIABLE _psvrecore_pkg_bin)
# Imported targets of other packages a library may name (found again by the game's configure step).
set(_psvrecore_pkg_external Threads::Threads)

# The package's name for a target of this build.
function(_psvrecore_pkg_name out real)
    if(real STREQUAL "psvrecore_game")
        set(${out} psvrecore::game PARENT_SCOPE)
    elseif(real STREQUAL "psvrecore_generated_code")
        set(${out} psvrecore::generated_code PARENT_SCOPE)
    elseif(real STREQUAL "psvrecore_shared")
        set(${out} psvrecore::shared PARENT_SCOPE)
    else()
        string(MAKE_C_IDENTIFIER "${real}" _id)
        set(${out} "psvrecore::_${_id}" PARENT_SCOPE)
    endif()
endfunction()

# An include folder of this build: its place in the package (src/<path in the checkout>, build/<path in the build
# folder>). Appends "<folder>|<place>" to _dirs in the caller's scope; sets _mapped.
macro(_psvrecore_pkg_map_dir _dir_in)
    set(_d "${_dir_in}")
    cmake_path(NORMAL_PATH _d)
    string(REGEX REPLACE "/$" "" _d "${_d}")
    cmake_path(IS_PREFIX _psvrecore_pkg_bin "${_d}" _in_bin)
    cmake_path(IS_PREFIX _psvrecore_pkg_src "${_d}" _in_src)
    if(_in_bin)  # (before the checkout: the build folder may be inside it)
        cmake_path(RELATIVE_PATH _d BASE_DIRECTORY "${_psvrecore_pkg_bin}" OUTPUT_VARIABLE _rel)
        set(_key "build/${_rel}")
    elseif(_in_src)
        cmake_path(RELATIVE_PATH _d BASE_DIRECTORY "${_psvrecore_pkg_src}" OUTPUT_VARIABLE _rel)
        set(_key "src/${_rel}")
    else()
        message(FATAL_ERROR "psvrecore package: include folder outside the checkout and the build folder: ${_d}")
    endif()
    string(REGEX REPLACE "/(\\.)?$" "" _key "${_key}")
    list(APPEND _dirs "${_d}|${_key}")
    set(_mapped "\${_IMPORT_PREFIX}/include/${_key}")
endmacro()

# Quoted for a CMake file: backslashes, quotes and variable references escaped.
function(_psvrecore_pkg_quote out value)
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\${" "\\\${" value "${value}")
    set(${out} "\"${value}\"" PARENT_SCOPE)
endfunction()

function(psvrecore_write_package)
    set(_queue psvrecore_game psvrecore_generated_code)
    set(_seen "")
    set(_dirs "")
    set(_externals "")
    set(_static_targets "")
    set(_targets_file "")
    set(_locations_file "")

    while(_queue)
        list(POP_FRONT _queue _t)
        if(_t IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen ${_t})
        get_target_property(_type ${_t} TYPE)
        _psvrecore_pkg_name(_name ${_t})
        string(MAKE_C_IDENTIFIER "${_t}" _id)

        if(_type STREQUAL "INTERFACE_LIBRARY")
            string(APPEND _targets_file "add_library(${_name} INTERFACE IMPORTED)\n")
        elseif(_type STREQUAL "SHARED_LIBRARY")
            string(APPEND _targets_file "add_library(${_name} SHARED IMPORTED)\n")
            if(WIN32)
                string(APPEND _locations_file "set(_psvrecore_location_${_id} \"bin/$<TARGET_FILE_NAME:${_t}>\")\n")
                string(APPEND _locations_file "set(_psvrecore_implib_${_id} \"lib/$<TARGET_LINKER_FILE_NAME:${_t}>\")\n")
                string(APPEND _targets_file
                    "set_target_properties(${_name} PROPERTIES\n"
                    "    IMPORTED_LOCATION \"\${_IMPORT_PREFIX}/\${_psvrecore_location_${_id}}\"\n"
                    "    IMPORTED_IMPLIB \"\${_IMPORT_PREFIX}/\${_psvrecore_implib_${_id}}\")\n")
            else()
                string(APPEND _locations_file "set(_psvrecore_soname_${_id} \"$<TARGET_SONAME_FILE_NAME:${_t}>\")\n")
                string(APPEND _targets_file
                    "set_target_properties(${_name} PROPERTIES\n"
                    "    IMPORTED_LOCATION \"\${_IMPORT_PREFIX}/lib/\${_psvrecore_soname_${_id}}\"\n"
                    "    IMPORTED_SONAME \"\${_psvrecore_soname_${_id}}\")\n")
            endif()
        elseif(_type STREQUAL "STATIC_LIBRARY")
            if(PSVRECORE_SHARED)
                # (reached only through $<COMPILE_ONLY>: its objects are in the shared library, a game takes its
                # headers and definitions)
                string(APPEND _targets_file "add_library(${_name} INTERFACE IMPORTED)\n")
            else()
                list(APPEND _static_targets ${_t})
                string(APPEND _locations_file "set(_psvrecore_location_${_id} \"lib/$<TARGET_FILE_NAME:${_t}>\")\n")
                string(APPEND _targets_file
                    "add_library(${_name} STATIC IMPORTED)\n"
                    "set_target_properties(${_name} PROPERTIES\n"
                    "    IMPORTED_LOCATION \"\${_IMPORT_PREFIX}/\${_psvrecore_location_${_id}}\"\n"
                    "    IMPORTED_LINK_INTERFACE_LANGUAGES \"C;CXX\")\n")
            endif()
        else()
            message(FATAL_ERROR "psvrecore package: ${_t} is a ${_type}, which the package does not carry")
        endif()

        foreach(_prop INTERFACE_INCLUDE_DIRECTORIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES INTERFACE_COMPILE_DEFINITIONS
                      INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_FEATURES INTERFACE_LINK_OPTIONS
                      INTERFACE_LINK_LIBRARIES INTERFACE_LINK_DIRECTORIES INTERFACE_SOURCES)
            get_target_property(_value ${_t} ${_prop})
            if(NOT _value)
                continue()
            endif()
            set(_out "")
            if(_prop MATCHES "INCLUDE_DIRECTORIES$")
                foreach(_e IN LISTS _value)
                    if(_e MATCHES "^\\$<BUILD_INTERFACE:(.*)>$")
                        string(REPLACE "$<SEMICOLON>" ";" _paths "${CMAKE_MATCH_1}")
                    elseif(_e MATCHES "^\\$<INSTALL_INTERFACE:")
                        continue()
                    elseif(_e MATCHES "[$<>]")
                        message(FATAL_ERROR "psvrecore package: ${_t}'s ${_prop} has ${_e}")
                    else()
                        set(_paths "${_e}")
                    endif()
                    foreach(_p IN LISTS _paths)
                        _psvrecore_pkg_map_dir("${_p}")
                        list(APPEND _out "${_mapped}")
                    endforeach()
                endforeach()
                list(JOIN _out ";" _joined)
                # (escaped as a whole, then the import prefix let through)
                _psvrecore_pkg_quote(_q "${_joined}")
                string(REPLACE "\\\${_IMPORT_PREFIX}" "\${_IMPORT_PREFIX}" _q "${_q}")
            elseif(_prop STREQUAL "INTERFACE_LINK_LIBRARIES")
                foreach(_e IN LISTS _value)
                    if(_e MATCHES "^\\$<(LINK_ONLY|COMPILE_ONLY|TARGET_NAME):([^$<>]+)>$")
                        set(_wrap "${CMAKE_MATCH_1}")
                        set(_inner "${CMAKE_MATCH_2}")
                    elseif(_e MATCHES "[$<>]")
                        message(FATAL_ERROR "psvrecore package: ${_t}'s ${_prop} has ${_e}")
                    else()
                        set(_wrap "")
                        set(_inner "${_e}")
                    endif()
                    if(TARGET ${_inner})
                        get_target_property(_real ${_inner} ALIASED_TARGET)
                        if(NOT _real)
                            set(_real ${_inner})
                        endif()
                        get_target_property(_imported ${_real} IMPORTED)
                        if(_imported)
                            if(NOT _inner IN_LIST _psvrecore_pkg_external)
                                message(FATAL_ERROR "psvrecore package: ${_t} links the imported target ${_inner}")
                            endif()
                            list(APPEND _externals ${_inner})
                            set(_item ${_inner})
                        else()
                            list(APPEND _queue ${_real})
                            _psvrecore_pkg_name(_item ${_real})
                        endif()
                    else()
                        set(_item "${_inner}")  # (a system library or a linker flag)
                    endif()
                    if(_wrap STREQUAL "LINK_ONLY" OR _wrap STREQUAL "COMPILE_ONLY")
                        set(_item "$<${_wrap}:${_item}>")
                    endif()
                    list(APPEND _out "${_item}")
                endforeach()
                list(JOIN _out ";" _joined)
                _psvrecore_pkg_quote(_q "${_joined}")
            else()
                string(FIND "${_value}" "${_psvrecore_pkg_src}/" _at_src)
                string(FIND "${_value}" "${_psvrecore_pkg_bin}/" _at_bin)
                if(NOT _at_src EQUAL -1 OR NOT _at_bin EQUAL -1)
                    message(FATAL_ERROR "psvrecore package: ${_t}'s ${_prop} names a path of this build: ${_value}")
                endif()
                _psvrecore_pkg_quote(_q "${_value}")
            endif()
            string(APPEND _targets_file "set_property(TARGET ${_name} PROPERTY ${_prop} ${_q})\n")
        endforeach()
    endwhile()

    # Headers: each include folder's headers at its place in the package.
    list(REMOVE_DUPLICATES _dirs)
    foreach(_entry IN LISTS _dirs)
        string(REPLACE "|" ";" _pair "${_entry}")
        list(GET _pair 0 _from)
        list(GET _pair 1 _to)
        install(DIRECTORY "${_from}/" DESTINATION "include/${_to}" COMPONENT psvrecore
            FILES_MATCHING PATTERN "*.h" PATTERN "*.hh" PATTERN "*.hpp" PATTERN "*.hxx" PATTERN "*.inl" PATTERN "*.inc"
                           PATTERN "*.ipp" PATTERN "*.def"
            PATTERN ".git" EXCLUDE)
    endforeach()

    # Libraries.
    if(PSVRECORE_SHARED)
        install(TARGETS psvrecore_shared RUNTIME DESTINATION bin COMPONENT psvrecore LIBRARY DESTINATION lib COMPONENT psvrecore
                ARCHIVE DESTINATION lib COMPONENT psvrecore)
        if(MSVC)
            install(FILES "$<TARGET_PDB_FILE:psvrecore_shared>" DESTINATION bin COMPONENT psvrecore OPTIONAL)
        endif()
    endif()
    foreach(_t IN LISTS _static_targets)
        install(FILES "$<TARGET_FILE:${_t}>" DESTINATION lib COMPONENT psvrecore)
    endforeach()
    # (built with the package even when nothing in "all" needs them)
    if(PSVRECORE_SHARED)
        list(APPEND _static_targets_built psvrecore_shared)
    endif()
    add_custom_target(psvrecore_package_libraries ALL DEPENDS ${_static_targets} ${_static_targets_built})

    # Host tools.
    if(PSVRECORE_BUILD_TOOLS)
        install(TARGETS psvinfo psvdis psvrecomp psvusse psvrender RUNTIME DESTINATION bin COMPONENT psvrecore)
        # (psvrecomp generate --replace-table <package>/share/psvrecore/host_functions.tsv)
        install(FILES "${PSVRECORE_DIR}/tools/psvrecomp/host_functions.tsv" DESTINATION share/psvrecore COMPONENT psvrecore)
    endif()

    # The functions a game calls.
    install(FILES "${PSVRECORE_DIR}/cmake/PsvrecoreCompilerOptions.cmake" "${PSVRECORE_DIR}/cmake/PsvrecorePackage.cmake"
                  "${PSVRECORE_DIR}/cmake/utf8.manifest"
            DESTINATION share/psvrecore/cmake COMPONENT psvrecore)

    # The targets, their files' names (known when generating) and the configuration.
    list(REMOVE_DUPLICATES _externals)
    file(WRITE "${_psvrecore_pkg_dir}/psvrecoreTargets.cmake"
        "# Generated by psvrecore's cmake/PsvrecoreInstall.cmake: psvrecore's targets, imported.\n${_targets_file}")
    file(GENERATE OUTPUT "${_psvrecore_pkg_dir}/psvrecoreLocations.cmake"
        CONTENT "# Generated by psvrecore's cmake/PsvrecoreInstall.cmake: the libraries' file names.\n${_locations_file}")

    set(PSVRECORE_PACKAGE_EXTERNALS "${_externals}")
    set(PSVRECORE_PACKAGE_SOURCE_DIR "${_psvrecore_pkg_src}")
    configure_file("${PSVRECORE_DIR}/cmake/psvrecoreConfig.cmake.in" "${_psvrecore_pkg_dir}/psvrecoreConfig.cmake" @ONLY)
    write_basic_package_version_file("${_psvrecore_pkg_dir}/psvrecoreConfigVersion.cmake"
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    configure_file("${PSVRECORE_DIR}/cmake/PsvrecoreBuildInfo.cmake.in" "${_psvrecore_pkg_dir}/PsvrecoreBuildInfo.cmake" @ONLY)
    install(FILES "${_psvrecore_pkg_dir}/psvrecoreConfig.cmake" "${_psvrecore_pkg_dir}/psvrecoreConfigVersion.cmake"
                  "${_psvrecore_pkg_dir}/psvrecoreTargets.cmake" "${_psvrecore_pkg_dir}/psvrecoreLocations.cmake"
            DESTINATION lib/cmake/psvrecore COMPONENT psvrecore)
    # (the commit the package was installed from, written when installing: psvrecoreBuildInfo.cmake)
    install(SCRIPT "${_psvrecore_pkg_dir}/PsvrecoreBuildInfo.cmake" COMPONENT psvrecore)
    message(STATUS "psvrecore: installable package (cmake --install <build> --component psvrecore), ${CMAKE_INSTALL_PREFIX}")
endfunction()

psvrecore_write_package()
