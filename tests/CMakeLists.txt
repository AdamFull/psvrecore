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
target_link_libraries(psvrecore_tests PRIVATE psvrecore::formats psvrecore::arm psvrecore::usse psvrecore::usse_interp psvrecore::gxm psvrecore::interp psvrecore_fuzz_code
                                              nx::foundation zlib-ng::zlibstatic)
psvrecore_target_defaults(psvrecore_tests)

add_test(NAME psvrecore_tests COMMAND psvrecore_tests)

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
    runtime/ngs_tests.cpp
    runtime/savedata_tests.cpp
    runtime/kernel_tests.cpp
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
)
target_include_directories(psvrecore_platform_tests PRIVATE "${_nxtest_dir}" "${PSVRECORE_DIR}/libs/gxm/src/hle")
target_link_libraries(psvrecore_platform_tests PRIVATE psvrecore::platform psvrecore::frontend nx::foundation)
psvrecore_target_defaults(psvrecore_platform_tests)
add_test(NAME psvrecore_platform_tests COMMAND psvrecore_platform_tests)
