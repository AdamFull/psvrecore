# What every psvrecore library shares: error handling and log categories.
add_library(psvrecore_core INTERFACE)
add_library(psvrecore::core ALIAS psvrecore_core)

target_include_directories(psvrecore_core INTERFACE include)
target_link_libraries(psvrecore_core INTERFACE nx::foundation)
