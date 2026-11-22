# The code generator's differential test corpus: random instruction blocks (fixed seed) translated at build time.
set(PSVRECORE_FUZZ_SEED "0x9E3779B97F4A7C15" CACHE STRING "seed of the generator's differential test corpus")
set(PSVRECORE_FUZZ_BLOCKS "2000" CACHE STRING "blocks in the generator's differential test corpus")
add_executable(fuzzgen fuzz/fuzzgen.cpp)
target_link_libraries(fuzzgen PRIVATE psvrecore::recomp)
psvrecore_target_defaults(fuzzgen)
set(_fuzz_dir "${CMAKE_CURRENT_BINARY_DIR}/fuzz")
set(_fuzz_sources "${_fuzz_dir}/fuzz_code_0.c" "${_fuzz_dir}/fuzz_module.c" "${_fuzz_dir}/fuzz_blocks.c")
add_custom_command(
    OUTPUT ${_fuzz_sources} "${_fuzz_dir}/fuzz_functions.h"
    COMMAND fuzzgen "${_fuzz_dir}" "${PSVRECORE_FUZZ_SEED}" "${PSVRECORE_FUZZ_BLOCKS}"
    DEPENDS fuzzgen
    COMMENT "Generating the code generator's differential test corpus"
    VERBATIM)
add_library(psvrecore_fuzz_code STATIC ${_fuzz_sources})
target_include_directories(psvrecore_fuzz_code PUBLIC "${_fuzz_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/fuzz")
target_link_libraries(psvrecore_fuzz_code PUBLIC psvrecore::recomp_support)
target_compile_definitions(psvrecore_fuzz_code PRIVATE PSV_ADDRESS_MASK=0xFFFF)
psvrecore_generated_code(psvrecore_fuzz_code)
# The Advanced SIMD corpus (fuzzgen --simd): blocks of one to three Advanced SIMD instructions, run from states whose
# lanes hold each element size's edge values (generator_tests.cpp).
set(PSVRECORE_FUZZ_SIMD_BLOCKS "8000" CACHE STRING "blocks in the generator's Advanced SIMD differential test corpus")
set(_fuzz_simd_sources "${_fuzz_dir}/fuzzsimd_code_0.c" "${_fuzz_dir}/fuzzsimd_module.c" "${_fuzz_dir}/fuzzsimd_blocks.c")
add_custom_command(
    OUTPUT ${_fuzz_simd_sources} "${_fuzz_dir}/fuzzsimd_functions.h"
    COMMAND fuzzgen "${_fuzz_dir}" "${PSVRECORE_FUZZ_SEED}" "${PSVRECORE_FUZZ_SIMD_BLOCKS}" --simd
    DEPENDS fuzzgen
    COMMENT "Generating the code generator's Advanced SIMD differential test corpus"
    VERBATIM)
add_library(psvrecore_fuzz_simd_code STATIC ${_fuzz_simd_sources})
target_include_directories(psvrecore_fuzz_simd_code PUBLIC "${_fuzz_dir}" "${CMAKE_CURRENT_SOURCE_DIR}/fuzz")
target_link_libraries(psvrecore_fuzz_simd_code PUBLIC psvrecore::recomp_support)
target_compile_definitions(psvrecore_fuzz_simd_code PRIVATE PSV_ADDRESS_MASK=0xFFFF)
psvrecore_generated_code(psvrecore_fuzz_simd_code)

# Unit tests: no game data and no SDK files. Written with nxtest, nx-foundation's test framework; its runner source
# is compiled in here so nx-foundation's own test suites (NX_FOUNDATION_TESTS) need not be configured.
set(_nxtest_dir "${PSVRECORE_DIR}/libs/nx-foundation/tests/framework")

add_executable(psvrecore_tests
    "${_nxtest_dir}/nxtest.cpp"
    core_tests.cpp
    address_space_tests.cpp
    formats_tests.cpp
    cpk_tests.cpp
    arm_tests.cpp
    analysis_tests.cpp
    flags_tests.cpp
    registers_tests.cpp
    vfp_tests.cpp
    generator_tests.cpp
    interp_tests.cpp
    float_tests.cpp
    vfp_typed_tests.cpp
    simd_tests.cpp
    usse_tests.cpp
    usse_interp_tests.cpp
    gxm_texture_tests.cpp
    gxm_translate_tests.cpp
    replace_tests.cpp
)
target_include_directories(psvrecore_tests PRIVATE "${_nxtest_dir}" codegen)
# (replace_tests checks recomp::kRuntimeHostFunctions against guest.h's declarations)
target_compile_definitions(psvrecore_tests PRIVATE PSVRECORE_GUEST_H="${CMAKE_CURRENT_SOURCE_DIR}/../libs/recomp/include/psvrecore/recomp/guest.h")
target_link_libraries(psvrecore_tests PRIVATE psvrecore::formats psvrecore::arm psvrecore::analysis psvrecore::recomp psvrecore::recomp_support psvrecore::usse psvrecore::usse_interp psvrecore::gxm psvrecore::interp psvrecore_fuzz_code psvrecore_fuzz_simd_code
                                              nx::foundation zlib-ng::zlibstatic)
psvrecore_target_defaults(psvrecore_tests)

add_test(NAME psvrecore_tests COMMAND psvrecore_tests)

# The generator's synthetic corpus (codegen/: compiled and hand-written ARM code, checked in as corpus_data.inc by
# scripts/codegen_corpus.py), translated at build time: whole functions run against the interpreter, through the
# production memory path (psvrecore_codegen_tests), through folded memory with the kept registers checked after each
# call (psvrecore_codegen_masked_tests), and with the registers in locals but no callee-saved knowledge
# (psvrecore_codegen_locals_tests: psvrecomp --no-callee-saved). scripts/codegen_metrics.py measures the same code
# (corpusgen writes it with the default file size there). PSVRECORE_CODEGEN_TEST_ARGS: more corpusgen options for the
# first two (--registers-in-context, --keep-registers loops...: for measuring or chasing a variant).
add_executable(corpusgen codegen/corpusgen.cpp)
target_include_directories(corpusgen PRIVATE codegen)
target_link_libraries(corpusgen PRIVATE psvrecore::recomp)
psvrecore_target_defaults(corpusgen)
# The masked configuration's code is generated with --full-exit: its test functions leave all four condition flags
# at return and the test compares them all; the other's leave only those a caller reads (see corpusgen.cpp).
set(PSVRECORE_CODEGEN_TEST_ARGS "" CACHE STRING "more corpusgen options for the synthetic corpus' code (tests/codegen)")
separate_arguments(_corpus_args NATIVE_COMMAND "${PSVRECORE_CODEGEN_TEST_ARGS}")
# PSVRECORE_CODEGEN_TEST_SPLIT=<k>: each module's code in k .c files (corpusgen --split), for measuring calls across
# files; 0, one file each.
set(PSVRECORE_CODEGEN_TEST_SPLIT 0 CACHE STRING "the synthetic corpus' code in this many .c files per module (0: one)")
set(_corpus_files 1)
if(PSVRECORE_CODEGEN_TEST_SPLIT GREATER 0)
    set(_corpus_files ${PSVRECORE_CODEGEN_TEST_SPLIT})
    list(APPEND _corpus_args --split ${PSVRECORE_CODEGEN_TEST_SPLIT})
endif()
math(EXPR _corpus_last "${_corpus_files} - 1")
set(_corpus_modules calls control flags integer integer_arm neon neon_locals regs vfp vfp_locals)  # (one per file in codegen/corpus)
foreach(_config "" _masked _locals)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/codegen${_config}")
    set(_corpus_sources${_config} "${_dir}/corpus_modules.c")
    set(_corpus_includes${_config} "")
    foreach(_m IN LISTS _corpus_modules)
        foreach(_k RANGE ${_corpus_last})
            list(APPEND _corpus_sources${_config} "${_dir}/syn_${_m}/syn_${_m}_code_${_k}.c")
        endforeach()
        list(APPEND _corpus_sources${_config} "${_dir}/syn_${_m}/syn_${_m}_module.c")
        list(APPEND _corpus_includes${_config} "${_dir}/syn_${_m}")
    endforeach()
endforeach()
add_custom_command(
    OUTPUT ${_corpus_sources}
    COMMAND corpusgen "${CMAKE_CURRENT_BINARY_DIR}/codegen" 0xFFFFFFFF ${_corpus_args}
    DEPENDS corpusgen "${CMAKE_CURRENT_SOURCE_DIR}/codegen/corpus_data.inc"
    COMMENT "Translating the code generator's synthetic corpus"
    VERBATIM)
add_custom_command(
    OUTPUT ${_corpus_sources_masked}
    COMMAND corpusgen "${CMAKE_CURRENT_BINARY_DIR}/codegen_masked" 0xFFFFFFFF --full-exit ${_corpus_args}
    DEPENDS corpusgen "${CMAKE_CURRENT_SOURCE_DIR}/codegen/corpus_data.inc"
    COMMENT "Translating the code generator's synthetic corpus (every flag at exit)"
    VERBATIM)
add_custom_command(
    OUTPUT ${_corpus_sources_locals}
    COMMAND corpusgen "${CMAKE_CURRENT_BINARY_DIR}/codegen_locals" 0xFFFFFFFF --no-callee-saved
    DEPENDS corpusgen "${CMAKE_CURRENT_SOURCE_DIR}/codegen/corpus_data.inc"
    COMMENT "Translating the code generator's synthetic corpus (no callee-saved knowledge)"
    VERBATIM)
add_library(psvrecore_codegen_code STATIC ${_corpus_sources})
add_library(psvrecore_codegen_code_masked STATIC ${_corpus_sources_masked})
add_library(psvrecore_codegen_code_locals STATIC ${_corpus_sources_locals})
target_compile_definitions(psvrecore_codegen_code_masked PRIVATE PSV_ADDRESS_MASK=0x7FFFF PSV_CHECK_KEPT_REGISTERS)
foreach(_config "" _masked _locals)
    target_include_directories(psvrecore_codegen_code${_config} PRIVATE ${_corpus_includes${_config}})
    target_link_libraries(psvrecore_codegen_code${_config} PUBLIC psvrecore::recomp_support)
    psvrecore_generated_code(psvrecore_codegen_code${_config})
    add_executable(psvrecore_codegen${_config}_tests "${_nxtest_dir}/nxtest.cpp" codegen/codegen_tests.cpp)
    target_include_directories(psvrecore_codegen${_config}_tests PRIVATE "${_nxtest_dir}" codegen)
    target_link_libraries(psvrecore_codegen${_config}_tests PRIVATE psvrecore::interp psvrecore_codegen_code${_config} nx::foundation)
    psvrecore_target_defaults(psvrecore_codegen${_config}_tests)
    add_test(NAME psvrecore_codegen${_config}_tests COMMAND psvrecore_codegen${_config}_tests)
    set_tests_properties(psvrecore_codegen${_config}_tests PROPERTIES SKIP_RETURN_CODE 77)
endforeach()
target_compile_definitions(psvrecore_codegen_masked_tests PRIVATE CODEGEN_TEST_MASK=0x7FFFF)
target_compile_definitions(psvrecore_codegen_locals_tests PRIVATE CODEGEN_TEST_NO_CALLEE_SAVED)

# Host implementations of guest functions (psvrecomp --replace; libs/recomp/src/host_functions.cpp): the psv_host_*
# functions against reference models on random buffers, and codegen/replace_module.hpp's synthetic module translated
# with two of its functions replaced (replacegen), its callers run against the interpreter. PSVRECORE_TEST_LIBC=<a
# libc.suprx or its ELF> at run time: the psv_host_* functions against that module's own exports in the interpreter
# too (local only: no Sony code in the repository).
add_executable(replacegen codegen/replacegen.cpp)
target_include_directories(replacegen PRIVATE codegen)
target_link_libraries(replacegen PRIVATE psvrecore::recomp)
psvrecore_target_defaults(replacegen)
set(_replace_dir "${CMAKE_CURRENT_BINARY_DIR}/replace/rep_mod")
add_custom_command(
    OUTPUT "${_replace_dir}/rep_mod_code_0.c" "${_replace_dir}/rep_mod_module.c"
    COMMAND replacegen "${CMAKE_CURRENT_BINARY_DIR}/replace"
    DEPENDS replacegen "${CMAKE_CURRENT_SOURCE_DIR}/codegen/replace_module.hpp"
    COMMENT "Translating the synthetic module with host implementations"
    VERBATIM)
add_library(psvrecore_replace_code STATIC "${_replace_dir}/rep_mod_code_0.c" "${_replace_dir}/rep_mod_module.c")
target_include_directories(psvrecore_replace_code PUBLIC "${_replace_dir}")
target_link_libraries(psvrecore_replace_code PUBLIC psvrecore::recomp_support)
psvrecore_generated_code(psvrecore_replace_code)
add_executable(psvrecore_host_functions_tests "${_nxtest_dir}/nxtest.cpp" codegen/host_functions_tests.cpp)
target_include_directories(psvrecore_host_functions_tests PRIVATE "${_nxtest_dir}" codegen)
target_link_libraries(psvrecore_host_functions_tests PRIVATE psvrecore::interp psvrecore::analysis psvrecore_replace_code nx::foundation)
psvrecore_target_defaults(psvrecore_host_functions_tests)
add_test(NAME psvrecore_host_functions_tests COMMAND psvrecore_host_functions_tests)
set_tests_properties(psvrecore_host_functions_tests PROPERTIES SKIP_RETURN_CODE 77)

# Same-source probes, built only by scripts/probe_check.py (they need the SDK and recompiled SDK code: local only).
set(PSVRECORE_PROBES_DIR "" CACHE PATH "folder of SDK-built probes and their recompiled code (scripts/probe_check.py)")
if(PSVRECORE_PROBES_DIR)
    add_subdirectory(probes)
endif()

# The runtime: its own executable (it reserves the guest address space, which the generator's tests also use).
# With it, runtime/spin_module.hpp's guest threads that wait for each other through memory, translated by the
# generator (spingen) and compiled as a game's code is.
add_executable(spingen runtime/spingen.cpp)
target_include_directories(spingen PRIVATE runtime)
target_link_libraries(spingen PRIVATE psvrecore::recomp)
psvrecore_target_defaults(spingen)
set(_spin_dir "${CMAKE_CURRENT_BINARY_DIR}/spin/spin_mod")
add_custom_command(
    OUTPUT "${_spin_dir}/spin_mod_code_0.c" "${_spin_dir}/spin_mod_module.c"
    COMMAND spingen "${CMAKE_CURRENT_BINARY_DIR}/spin"
    DEPENDS spingen "${CMAKE_CURRENT_SOURCE_DIR}/runtime/spin_module.hpp"
    COMMENT "Translating the synthetic module of waiting threads"
    VERBATIM)
add_library(psvrecore_runtime_test_guest STATIC runtime/runtime_guest.c "${_spin_dir}/spin_mod_code_0.c"
                                                "${_spin_dir}/spin_mod_module.c")
target_include_directories(psvrecore_runtime_test_guest PRIVATE "${_spin_dir}")
target_link_libraries(psvrecore_runtime_test_guest PUBLIC psvrecore::recomp_support)
psvrecore_generated_code(psvrecore_runtime_test_guest)

add_executable(psvrecore_runtime_tests
    "${_nxtest_dir}/nxtest.cpp"
    runtime/audio_tests.cpp
    runtime/hle_tests.cpp
    runtime/input_tests.cpp
    runtime/media_tests.cpp
    runtime/font_tests.cpp
    runtime/ngs_tests.cpp
    runtime/savedata_tests.cpp
    runtime/kernel_tests.cpp
    runtime/system_tests.cpp
    runtime/network_tests.cpp
    runtime/psn_tests.cpp
)
target_include_directories(psvrecore_runtime_tests PRIVATE "${_nxtest_dir}" runtime)
target_link_libraries(psvrecore_runtime_tests PRIVATE psvrecore::runtime psvrecore_runtime_test_guest nx::foundation)
psvrecore_target_defaults(psvrecore_runtime_tests)
add_test(NAME psvrecore_runtime_tests COMMAND psvrecore_runtime_tests)

# The Vulkan layer on a real device (skipped without one).
add_executable(psvrecore_gpu_tests
    "${_nxtest_dir}/nxtest.cpp"
    gpu/vulkan_tests.cpp
)
target_include_directories(psvrecore_gpu_tests PRIVATE "${_nxtest_dir}" "${PSVRECORE_DIR}/third_party/SPIRV-Headers/include")
target_link_libraries(psvrecore_gpu_tests PRIVATE psvrecore::vulkan psvrecore::gxm nx::foundation)
psvrecore_target_defaults(psvrecore_gpu_tests)
add_test(NAME psvrecore_gpu_tests COMMAND psvrecore_gpu_tests)

# Test programs on the runtime, built only by scripts/program_check.py (they need the SDK: local only).
set(PSVRECORE_PROGRAMS_DIR "" CACHE PATH "folder of SDK-built test programs and their recompiled code (scripts/program_check.py)")
if(PSVRECORE_PROGRAMS_DIR)
    add_subdirectory(programs)
endif()

# The SDK's GXM samples, built only by scripts/sample_check.py (they need the SDK: local only).
set(PSVRECORE_SAMPLES_DIR "" CACHE PATH "folder of SDK-built GXM samples and their recompiled code (scripts/sample_check.py)")
if(PSVRECORE_SAMPLES_DIR)
    add_subdirectory(samples)
endif()

# The platform layer (SDL3): the controls, the audio device (SDL's disk driver) and the window (skipped without a
# display or a Vulkan device); the frontend's settings, command line and scripts.
add_executable(psvrecore_platform_tests
    "${_nxtest_dir}/nxtest.cpp"
    platform/platform_tests.cpp
    platform/frontend_tests.cpp
    platform/gxm_hle_tests.cpp
)
target_include_directories(psvrecore_platform_tests PRIVATE "${_nxtest_dir}" "${PSVRECORE_DIR}/libs/gxm/src/hle" runtime)
target_link_libraries(psvrecore_platform_tests PRIVATE psvrecore::platform psvrecore::frontend nx::foundation)
psvrecore_target_defaults(psvrecore_platform_tests)
add_test(NAME psvrecore_platform_tests COMMAND psvrecore_platform_tests)

# (nxtest exits with 77 when every case was skipped: no Vulkan device, no window)
set_tests_properties(psvrecore_tests psvrecore_runtime_tests psvrecore_gpu_tests psvrecore_platform_tests PROPERTIES SKIP_RETURN_CODE 77)

# A program linked as a game's executable is, on the shared runtime library (PSVRECORE_SHARED): the start-up check
# and what crosses the library's interface. Built next to the library, which it loads.
if(PSVRECORE_SHARED)
    add_executable(psvrecore_shared_tests shared/shared_tests.cpp)
    target_link_libraries(psvrecore_shared_tests PRIVATE psvrecore::game)
    psvrecore_target_defaults(psvrecore_shared_tests)
    set_target_properties(psvrecore_shared_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    # (as a game's program: scripts/package_tests.py packages it and runs it from the package's folder)
    psvrecore_game_package(psvrecore_shared_tests FOLDER "${CMAKE_CURRENT_BINARY_DIR}/package")
    add_test(NAME psvrecore_shared_tests COMMAND psvrecore_shared_tests WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endif()
