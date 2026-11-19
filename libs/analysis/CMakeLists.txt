# Code discovery: modules loaded at their guest addresses, functions, control flow and table branches; each
# function's control-flow graph and the condition flags' liveness over it.
add_library(psvrecore_analysis STATIC
    src/cfg.cpp
    src/flags.cpp
    src/flow.cpp
    src/image.cpp
    src/program.cpp
    src/registers.cpp
)
add_library(psvrecore::analysis ALIAS psvrecore_analysis)

target_include_directories(psvrecore_analysis PUBLIC include)
target_link_libraries(psvrecore_analysis PUBLIC psvrecore::formats psvrecore::arm)
psvrecore_target_defaults(psvrecore_analysis)
