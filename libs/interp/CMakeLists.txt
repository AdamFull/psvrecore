# The reference interpreter: the oracle of the code generator's differential tests. Never linked into a game.
add_library(psvrecore_interp STATIC
    src/interp_core.cpp
    src/interp_memory.cpp
    src/interp_simd.cpp
    src/interp_vfp.cpp
    src/interpreter.cpp
)
add_library(psvrecore::interp ALIAS psvrecore_interp)
target_include_directories(psvrecore_interp PUBLIC include)
target_link_libraries(psvrecore_interp PUBLIC psvrecore::arm psvrecore::recomp_support)
psvrecore_target_defaults(psvrecore_interp)
