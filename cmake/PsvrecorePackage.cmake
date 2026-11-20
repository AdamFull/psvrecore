# What a game's program takes from its packaging (scripts/package.py), made from the game's sce_sys into a folder the
# game's repository never commits (generated/package/ by default):
#
#   psvrecore_game_package(<target> [FOLDER <folder>])
#
# - Windows: <folder>/<target>.rc compiled into the program when it is there: the game's icon (Explorer's, the
#   window's: SDL takes the program's first icon) and its version information (title, title ID, APP_VER). Without
#   it (no package made yet) the program builds as before.
# - Linux: the program finds the runtime library next to itself ($ORIGIN), so a package folder runs wherever it is
#   copied.
# - Windows and Linux: <folder>/launch.png (the launch screen, made from the game's LiveArea) copied beside the
#   program after each build, when it is there, where the frontend looks for it.
# The cache variable PSVRECORE_PACKAGE_FOLDER overrides the folder.

function(psvrecore_game_package target)
    cmake_parse_arguments(PARSE_ARGV 1 ARG "" "FOLDER" "")
    if(PSVRECORE_PACKAGE_FOLDER)
        set(_folder "${PSVRECORE_PACKAGE_FOLDER}")
    elseif(ARG_FOLDER)
        set(_folder "${ARG_FOLDER}")
    else()
        set(_folder "${CMAKE_CURRENT_SOURCE_DIR}/generated/package")
    endif()
    if(WIN32)
        # (looked for when configuring: the package script configures after writing it)
        set(_rc "${_folder}/${target}.rc")
        if(EXISTS "${_rc}")
            target_sources(${target} PRIVATE "${_rc}")
            if(EXISTS "${_folder}/${target}.ico")
                set_source_files_properties("${_rc}" PROPERTIES OBJECT_DEPENDS "${_folder}/${target}.ico")
            endif()
            message(STATUS "${target}: icon and version information from ${_rc}")
        endif()
    elseif(UNIX AND NOT APPLE AND NOT ANDROID)
        # (the build folder's own paths made relative to the program: $ORIGIN, where the library is beside it)
        set_target_properties(${target} PROPERTIES BUILD_RPATH_USE_ORIGIN ON INSTALL_RPATH "\$ORIGIN")
    endif()
    # (built against the installed package: the runtime library copied beside the program, where a build of psvrecore
    # in the game's own build folder puts it)
    if(PSVRECORE_RUNTIME_FILES)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different ${PSVRECORE_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
    if(NOT ANDROID AND EXISTS "${_folder}/launch.png")
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_folder}/launch.png" "$<TARGET_FILE_DIR:${target}>/launch.png"
            VERBATIM)
        message(STATUS "${target}: launch screen ${_folder}/launch.png")
    endif()
endfunction()
