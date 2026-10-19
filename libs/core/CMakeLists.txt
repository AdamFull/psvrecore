# What every psvrecore library shares: error handling, log categories, binary readers, whole-file I/O, text and
# command-line helpers.
add_library(psvrecore_core STATIC
    src/io/file.cpp
    src/io/png.cpp
)
add_library(psvrecore::core ALIAS psvrecore_core)

target_include_directories(psvrecore_core PUBLIC include)
target_link_libraries(psvrecore_core PUBLIC nx::foundation)
psvrecore_target_defaults(psvrecore_core)
