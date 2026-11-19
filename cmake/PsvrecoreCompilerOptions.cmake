# Common compile options for first-party targets. Recompiled game code gets its own set on top when the game target
# is added (no warnings, wrapping integer arithmetic, no strict aliasing).

include(CheckCCompilerFlag)

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

# Options for generated code only (the defaults change nothing): the instruction set it is compiled for, ThinLTO, and
# how much debug information it carries. See docs/BUILDING.md.
set(PSVRECORE_GENERATED_CODE_ARCH "" CACHE STRING
    "-march for generated code (x64: e.g. x86-64-v2, x86-64-v3; arm64: e.g. armv8.2-a); empty: the toolchain's default")
set(PSVRECORE_GENERATED_CODE_CPU "" CACHE STRING "-mcpu for generated code (arm64: e.g. cortex-a76); empty: none")
option(PSVRECORE_GENERATED_CODE_LTO "ThinLTO across generated code (clang with lld or lld-link)" OFF)
set(PSVRECORE_GENERATED_CODE_DEBUG "lines" CACHE STRING
    "debug information of generated code in builds that have it: lines (function names and line tables), full, none")
set_property(CACHE PSVRECORE_GENERATED_CODE_DEBUG PROPERTY STRINGS lines full none)
option(PSVRECORE_GENERATED_CODE_CHECK_KEPT
    "generated code checks after each direct call that the registers it kept in locals are what the callee left" OFF)
# arm64 only: generated code's floating point as the host's instructions (PSV_NATIVE_FLOAT in guest.h), the runtime
# keeping the host's FPCR mirroring the guest's FPSCR (runtime/float_env.hpp). Not verified on a device yet: see
# docs/BUILDING.md for the device test that checks it.
option(PSVRECORE_GENERATED_CODE_NATIVE_FLOAT
    "arm64: generated code's floating point as host instructions under an FPCR that mirrors the guest FPSCR (unverified on hardware)" OFF)
if(PSVRECORE_GENERATED_CODE_NATIVE_FLOAT AND NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    message(FATAL_ERROR "PSVRECORE_GENERATED_CODE_NATIVE_FLOAT is for arm64 targets (this is ${CMAKE_SYSTEM_PROCESSOR})")
endif()

# The cache options above (each changes nothing when left at its default). Apart from psvrecore_generated_code so that
# scripts/codegen_metrics.py, which reads that function's options, measures the defaults; it takes these as --flags-*.
function(_psvrecore_generated_code_options target)
    if(MSVC)
        set(_clang /clang:)
    else()
        set(_clang "")
    endif()
    if(PSVRECORE_GENERATED_CODE_ARCH)
        target_compile_options(${target} PRIVATE ${_clang}-march=${PSVRECORE_GENERATED_CODE_ARCH})
    endif()
    if(PSVRECORE_GENERATED_CODE_CPU)
        target_compile_options(${target} PRIVATE ${_clang}-mcpu=${PSVRECORE_GENERATED_CODE_CPU})
    endif()
    # (after the configuration's -g or /Zi: the last one counts)
    if(PSVRECORE_GENERATED_CODE_DEBUG STREQUAL "lines")
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo,Debug>:${_clang}-gline-tables-only>)
        else()
            target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo,Debug>:-g1>)
        endif()
    elseif(PSVRECORE_GENERATED_CODE_DEBUG STREQUAL "none")
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo,Debug>:${_clang}-g0>)
    elseif(NOT PSVRECORE_GENERATED_CODE_DEBUG STREQUAL "full")
        message(FATAL_ERROR "PSVRECORE_GENERATED_CODE_DEBUG: lines, full or none, not ${PSVRECORE_GENERATED_CODE_DEBUG}")
    endif()
    if(PSVRECORE_GENERATED_CODE_CHECK_KEPT)
        target_compile_definitions(${target} PRIVATE PSV_CHECK_KEPT_REGISTERS)
    endif()
    # (and nothing assumed about how denormals compare: the FPCR's FZ decides at run time)
    if(PSVRECORE_GENERATED_CODE_NATIVE_FLOAT)
        target_compile_definitions(${target} PRIVATE PSV_NATIVE_FLOAT)
        check_c_compiler_flag(-fdenormal-fp-math=dynamic _psvrecore_has_dynamic_denormals)
        if(NOT _psvrecore_has_dynamic_denormals)
            message(FATAL_ERROR "PSVRECORE_GENERATED_CODE_NATIVE_FLOAT needs clang's -fdenormal-fp-math=dynamic (clang 17 or newer)")
        endif()
        target_compile_options(${target} PRIVATE -fdenormal-fp-math=dynamic)
    endif()
    if(PSVRECORE_GENERATED_CODE_LTO)
        if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang")
            message(FATAL_ERROR "PSVRECORE_GENERATED_CODE_LTO needs clang (and lld or lld-link)")
        endif()
        target_compile_options(${target} PRIVATE -flto=thin)
        if(NOT MSVC)
            target_link_options(${target} INTERFACE -flto=thin)
        endif()
    endif()
endfunction()

# For targets compiling generated code (psvrecomp's output): integer arithmetic wraps as the guest's does, guest
# memory is accessed through any type, floating point is exactly as written, and the warnings generated code would
# raise (unused labels and temporaries) are off. Unwind tables let the runtime end a guest thread from inside the
# calls it made (a C++ exception through the generated C frames); Windows x64 code always has them.
#
# And what generated code does not need: stack protectors (a canary in most files, for the guest's few local arrays);
# exported symbols (nothing outside the binary it is linked into calls a generated function by name: the game reaches
# a module through its PsvModuleCode, the runtime through the module's function table), so they are hidden and calls
# between them are direct, not through the PLT; arm64's out-of-line atomics (a call for each LDREX/STREX and swap,
# which checks the CPU's LSE support every time). clang-cl's RelWithDebInfo inlines only functions marked inline
# (/Ob1); /Ob2 inlines as -O2 does elsewhere. (Measured and left out, as they change nothing in generated code:
# -U_FORTIFY_SOURCE, since it calls no fortified libc function, and -fno-semantic-interposition, as hidden symbols
# are not interposable anyway. docs/CODEGEN_METRICS.md has the numbers.)
function(psvrecore_generated_code target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /clang:-fwrapv /clang:-fno-strict-aliasing /clang:-ffp-contract=off
                                                 /clang:-fno-fast-math /clang:-fno-math-errno /clang:-w /GS-
                                                 $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    else()
        target_compile_options(${target} PRIVATE -fwrapv -fno-strict-aliasing -ffp-contract=off -fno-fast-math -fno-math-errno
                                                 -w -funwind-tables)
        target_compile_options(${target} PRIVATE -fno-stack-protector -fvisibility=hidden)
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
            check_c_compiler_flag(-mno-outline-atomics _psvrecore_has_no_outline_atomics)
            if(_psvrecore_has_no_outline_atomics)
                target_compile_options(${target} PRIVATE -mno-outline-atomics)
            endif()
        endif()
        if(ANDROID)
            # (the shared library the code ends up in: references to its own symbols bound inside it, the runtime's
            # too when it is linked in statically)
            target_link_options(${target} INTERFACE "LINKER:-Bsymbolic")
        endif()
    endif()
    _psvrecore_generated_code_options(${target})
endfunction()
