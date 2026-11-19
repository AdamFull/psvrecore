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
    generator_tests.cpp
    interp_tests.cpp
    float_tests.cpp
    simd_tests.cpp
    usse_tests.cpp
    usse_interp_tests.cpp
    gxm_texture_tests.cpp
    gxm_translate_tests.cpp
)
target_include_directories(psvrecore_tests PRIVATE "${_nxtest_dir}")
target_link_libraries(psvrecore_tests PRIVATE psvrecore::formats psvrecore::arm psvrecore::analysis psvrecore::usse psvrecore::usse_interp psvrecore::gxm psvrecore::interp psvrecore_fuzz_code
                                              nx::foundation zlib-ng::zlibstatic)
psvrecore_target_defaults(psvrecore_tests)

add_test(NAME psvrecore_tests COMMAND psvrecore_tests)

# The generator's synthetic corpus (codegen/: compiled and hand-written ARM code, checked in as corpus_data.inc by
# scripts/codegen_corpus.py), translated at build time: whole functions run against the interpreter, through the
# production memory path (psvrecore_codegen_tests) and through folded memory (psvrecore_codegen_masked_tests).
# scripts/codegen_metrics.py measures the same code (corpusgen writes it with the default file size there).
add_executable(corpusgen codegen/corpusgen.cpp)
target_include_directories(corpusgen PRIVATE codegen)
target_link_libraries(corpusgen PRIVATE psvrecore::recomp)
psvrecore_target_defaults(corpusgen)
# The masked configuration's code is generated with --full-exit: its test functions leave all four condition flags
# at return and the test compares them all; the other's leave only those a caller reads (see corpusgen.cpp).
set(_corpus_modules calls control flags integer integer_arm neon vfp)  # (one per file in codegen/corpus)
foreach(_config "" _masked)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/codegen${_config}")
    set(_corpus_sources${_config} "${_dir}/corpus_modules.c")
    set(_corpus_includes${_config} "")
    foreach(_m IN LISTS _corpus_modules)
        list(APPEND _corpus_sources${_config} "${_dir}/syn_${_m}/syn_${_m}_code_0.c" "${_dir}/syn_${_m}/syn_${_m}_module.c")
        list(APPEND _corpus_includes${_config} "${_dir}/syn_${_m}")
    endforeach()
endforeach()
add_custom_command(
    OUTPUT ${_corpus_sources}
    COMMAND corpusgen "${CMAKE_CURRENT_BINARY_DIR}/codegen" 0xFFFFFFFF
    DEPENDS corpusgen "${CMAKE_CURRENT_SOURCE_DIR}/codegen/corpus_data.inc"
    COMMENT "Translating the code generator's synthetic corpus"
    VERBATIM)
add_custom_command(
    OUTPUT ${_corpus_sources_masked}
    COMMAND corpusgen "${CMAKE_CURRENT_BINARY_DIR}/codegen_masked" 0xFFFFFFFF --full-exit
    DEPENDS corpusgen "${CMAKE_CURRENT_SOURCE_DIR}/codegen/corpus_data.inc"
    COMMENT "Translating the code generator's synthetic corpus (every flag at exit)"
    VERBATIM)
add_library(psvrecore_codegen_code STATIC ${_corpus_sources})
add_library(psvrecore_codegen_code_masked STATIC ${_corpus_sources_masked})
target_compile_definitions(psvrecore_codegen_code_masked PRIVATE PSV_ADDRESS_MASK=0x7FFFF)
foreach(_config "" _masked)
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

# Same-source probes, built only by scripts/probe_check.py (they need the SDK and recompiled SDK code: local only).
set(PSVRECORE_PROBES_DIR "" CACHE PATH "folder of SDK-built probes and their recompiled code (scripts/probe_check.py)")
if(PSVRECORE_PROBES_DIR)
    add_subdirectory(probes)
endif()

# The runtime: its own executable (it reserves the guest address space, which the generator's tests also use).
add_library(psvrecore_runtime_test_guest STATIC runtime/runtime_guest.c)
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
