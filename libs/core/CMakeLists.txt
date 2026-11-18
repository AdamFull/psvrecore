# What every psvrecore library shares: error handling, log categories, binary readers, whole-file I/O, text and
# command-line helpers, the profiler's hooks.
add_library(psvrecore_core STATIC
    src/io/file.cpp
    src/io/png.cpp
    src/profile.cpp
)
add_library(psvrecore::core ALIAS psvrecore_core)

target_include_directories(psvrecore_core PUBLIC include)
target_link_libraries(psvrecore_core PUBLIC nx::foundation)
# (the profiler's hooks, psvrecore/profile.hpp: Tracy's client is nx-foundation's, linked through it)
if(PSVRECORE_TRACY)
    target_compile_definitions(psvrecore_core PUBLIC PSVRECORE_TRACY=1)
endif()
psvrecore_target_defaults(psvrecore_core)
