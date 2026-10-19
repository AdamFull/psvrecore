# The program a game becomes: its settings and command line, the window, sound and controls (platform), and the
# runtime with the GXM layer running the game's recompiled code.

add_library(psvrecore_frontend STATIC
    src/frontend.cpp
    src/script.cpp
    src/settings.cpp
)
add_library(psvrecore::frontend ALIAS psvrecore_frontend)
target_include_directories(psvrecore_frontend PUBLIC include)
target_link_libraries(psvrecore_frontend PUBLIC psvrecore::platform psvrecore::gxm_hle psvrecore::runtime)
psvrecore_target_defaults(psvrecore_frontend)
