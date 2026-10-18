# Unit tests: no game data and no SDK files. Written with nxtest, nx-foundation's test framework; its runner source
# is compiled in here so nx-foundation's own test suites (NX_FOUNDATION_TESTS) need not be configured.
set(_nxtest_dir "${PSVRECORE_DIR}/libs/nx-foundation/tests/framework")

add_executable(psvrecore_tests
    "${_nxtest_dir}/nxtest.cpp"
    core_tests.cpp
    address_space_tests.cpp
)
target_include_directories(psvrecore_tests PRIVATE "${_nxtest_dir}")
target_link_libraries(psvrecore_tests PRIVATE psvrecore::core nx::foundation)
psvrecore_target_defaults(psvrecore_tests)

add_test(NAME psvrecore_tests COMMAND psvrecore_tests)
