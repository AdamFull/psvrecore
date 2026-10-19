# USSE (the Vita GPU's shader instructions): GXP files, decoding, disassembly.
add_library(psvrecore_usse STATIC
    src/common.cpp
    src/constants.cpp
    src/decode_alu.cpp
    src/decode_dual.cpp
    src/decode_other.cpp
    src/format.cpp
    src/gxp.cpp
    src/program.cpp
)
add_library(psvrecore::usse ALIAS psvrecore_usse)

target_include_directories(psvrecore_usse PUBLIC include)
target_link_libraries(psvrecore_usse PUBLIC psvrecore::core)
psvrecore_target_defaults(psvrecore_usse)
