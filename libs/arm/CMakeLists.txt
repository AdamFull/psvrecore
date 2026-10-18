# ARMv7-A decoding (ARM and Thumb, VFPv3, Advanced SIMD) and disassembly.
add_library(psvrecore_arm STATIC
    src/decode_arm.cpp
    src/decode_thumb.cpp
    src/decode_vfp_simd.cpp
    src/format.cpp
)
add_library(psvrecore::arm ALIAS psvrecore_arm)

target_include_directories(psvrecore_arm PUBLIC include)
target_link_libraries(psvrecore_arm PUBLIC psvrecore::core)
psvrecore_target_defaults(psvrecore_arm)
psvrecore_full_inlining(psvrecore_arm)
