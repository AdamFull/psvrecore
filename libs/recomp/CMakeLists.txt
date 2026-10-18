# The code generator (host tool library), and the support code recompiled programs link: the guest interface
# (guest.h), the function registry and traps.
add_library(psvrecore_recomp STATIC
    src/emit_core.cpp
    src/emit_memory.cpp
    src/emit_simd.cpp
    src/emit_vfp.cpp
    src/generator.cpp
)
add_library(psvrecore::recomp ALIAS psvrecore_recomp)
target_include_directories(psvrecore_recomp PUBLIC include)
target_link_libraries(psvrecore_recomp PUBLIC psvrecore::analysis)
psvrecore_target_defaults(psvrecore_recomp)

add_library(psvrecore_recomp_support STATIC
    src/support.cpp
)
add_library(psvrecore::recomp_support ALIAS psvrecore_recomp_support)
target_include_directories(psvrecore_recomp_support PUBLIC include)
target_link_libraries(psvrecore_recomp_support PUBLIC psvrecore::core)
psvrecore_target_defaults(psvrecore_recomp_support)
