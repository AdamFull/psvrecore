# File formats of Vita software: SELF, ELF and stub archives, SCE module information and imports/exports,
# SCE relocations, SFO, CPK archives (CRI), and the NID database.
add_library(psvrecore_formats STATIC
    src/cpk.cpp
    src/elf.cpp
    src/module.cpp
    src/nids.cpp
    src/reloc.cpp
    src/self.cpp
    src/sfo.cpp
)
add_library(psvrecore::formats ALIAS psvrecore_formats)

target_include_directories(psvrecore_formats PUBLIC include)
# zlib-ng (native zng_ API) is built by nx-foundation's third-party tree; SELF segments may be zlib-compressed.
target_link_libraries(psvrecore_formats PUBLIC psvrecore::core PRIVATE zlib-ng::zlibstatic)
psvrecore_target_defaults(psvrecore_formats)
