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
    arm_tests.cpp
    generator_tests.cpp
    interp_tests.cpp
)
target_include_directories(psvrecore_tests PRIVATE "${_nxtest_dir}")
target_link_libraries(psvrecore_tests PRIVATE psvrecore::formats psvrecore::arm psvrecore::interp psvrecore_fuzz_code
                                              nx::foundation zlib-ng::zlibstatic)
psvrecore_target_defaults(psvrecore_tests)

add_test(NAME psvrecore_tests COMMAND psvrecore_tests)
