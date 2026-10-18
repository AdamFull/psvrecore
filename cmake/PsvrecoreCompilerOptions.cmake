# Common compile options for first-party targets. Recompiled game code gets its own set on top when the game target
# is added (no warnings, wrapping integer arithmetic, no strict aliasing).

function(psvrecore_target_defaults target)
    if(MSVC)  # MSVC or clang-cl
        # Vulkan's VkFoo{VK_STRUCTURE_TYPE_FOO} idiom leaves the remaining fields zeroed on purpose.
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive- -Wno-missing-field-initializers)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)
    endif()
    if(NOT WIN32)
        target_compile_definitions(${target} PRIVATE _FILE_OFFSET_BITS=64)
    endif()
    # Floating point as written: no fused multiply-add. Clang fuses a*b+c by default where the CPU has it (arm64
    # always); the guest's VFP/NEON round each operation separately, and recompiled code and the runtime's CPU paths
    # must give the same results on every host.
    if(MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        target_compile_options(${target} PRIVATE /clang:-ffp-contract=off)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -ffp-contract=off)
    endif()
    # Windows executables run with the UTF-8 code page (argv, file names), see utf8.manifest.
    get_target_property(_psvrecore_type ${target} TYPE)
    if(WIN32 AND _psvrecore_type STREQUAL "EXECUTABLE")
        target_sources(${target} PRIVATE "${PSVRECORE_DIR}/cmake/utf8.manifest")
    endif()
endfunction()

# For targets with hot inner loops: RelWithDebInfo's /Ob1 on clang-cl inlines only functions marked inline, so
# templates and small helpers (nx::simd's among them) would stay calls. /Ob2 inlines as -O2 does elsewhere.
function(psvrecore_full_inlining target)
    if(MSVC)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
endfunction()

# For targets compiling generated code (psvrecomp's output): integer arithmetic wraps as the guest's does, guest
# memory is accessed through any type, floating point is exactly as written, and the warnings generated code would
# raise (unused labels and temporaries) are off. Unwind tables let the runtime end a guest thread from inside the
# calls it made (a C++ exception through the generated C frames); Windows x64 code always has them.
function(psvrecore_generated_code target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /clang:-fwrapv /clang:-fno-strict-aliasing /clang:-ffp-contract=off
                                                 /clang:-fno-fast-math /clang:-w)
    else()
        target_compile_options(${target} PRIVATE -fwrapv -fno-strict-aliasing -ffp-contract=off -fno-fast-math -w
                                                 -funwind-tables)
    endif()
endfunction()
