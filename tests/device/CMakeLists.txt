# Tests built for an arm64 device and run there by hand (adb push, adb shell: docs/BUILDING.md, "Device tests"),
# built with generated code's options (psvrecore_generated_code: native floating point when
# PSVRECORE_GENERATED_CODE_NATIVE_FLOAT is on, as a game's code would be):
# - psvrecore_float_device_tests: guest.h's typed floating-point operations against the interpreter (vfp_typed_tests.cpp)
#   and the hand-computed float cases (float_tests.cpp), with the FPCR mirroring each FPSCR mode;
# - psvrecore_codegen_device_tests: the code generator's synthetic corpus run against the interpreter
#   (codegen/codegen_tests.cpp), translated on the build host by its corpusgen (PSVRECORE_HOST_CORPUSGEN: the path of
#   a corpusgen built for the host, e.g. build/windows-x64-release/tests/corpusgen.exe); left out without one.
set(_nxtest_dir "${PSVRECORE_DIR}/libs/nx-foundation/tests/framework")
set(_tests "${PSVRECORE_DIR}/tests")

add_executable(psvrecore_float_device_tests "${_nxtest_dir}/nxtest.cpp" "${_tests}/vfp_typed_tests.cpp" "${_tests}/float_tests.cpp")
target_include_directories(psvrecore_float_device_tests PRIVATE "${_nxtest_dir}")
target_link_libraries(psvrecore_float_device_tests PRIVATE psvrecore::interp psvrecore::arm psvrecore::recomp_support nx::foundation)
psvrecore_target_defaults(psvrecore_float_device_tests)
psvrecore_generated_code(psvrecore_float_device_tests)

set(PSVRECORE_HOST_CORPUSGEN "" CACHE FILEPATH "corpusgen built for the build host, for the device's codegen tests")
if(PSVRECORE_HOST_CORPUSGEN)
    set(_dir "${CMAKE_CURRENT_BINARY_DIR}/codegen")
    set(_sources "${_dir}/corpus_modules.c")
    set(_includes "")
    set(_corpus_modules calls control flags integer integer_arm neon regs vfp vfp_locals)  # (as tests/CMakeLists.txt)
    foreach(_m IN LISTS _corpus_modules)
        list(APPEND _sources "${_dir}/syn_${_m}/syn_${_m}_code_0.c" "${_dir}/syn_${_m}/syn_${_m}_module.c")
        list(APPEND _includes "${_dir}/syn_${_m}")
    endforeach()
    add_custom_command(
        OUTPUT ${_sources}
        COMMAND "${PSVRECORE_HOST_CORPUSGEN}" "${_dir}" 0xFFFFFFFF
        DEPENDS "${PSVRECORE_HOST_CORPUSGEN}" "${_tests}/codegen/corpus_data.inc"
        COMMENT "Translating the code generator's synthetic corpus (host corpusgen)"
        VERBATIM)
    add_library(psvrecore_codegen_device_code STATIC ${_sources})
    target_include_directories(psvrecore_codegen_device_code PRIVATE ${_includes})
    target_link_libraries(psvrecore_codegen_device_code PUBLIC psvrecore::recomp_support)
    psvrecore_generated_code(psvrecore_codegen_device_code)
    add_executable(psvrecore_codegen_device_tests "${_nxtest_dir}/nxtest.cpp" "${_tests}/codegen/codegen_tests.cpp")
    target_include_directories(psvrecore_codegen_device_tests PRIVATE "${_nxtest_dir}" "${_tests}/codegen")
    target_link_libraries(psvrecore_codegen_device_tests PRIVATE psvrecore::interp psvrecore_codegen_device_code nx::foundation)
    psvrecore_target_defaults(psvrecore_codegen_device_tests)
    # (PSV_NATIVE_FLOAT there too: the test sets the FPCR mirror around each run, as the runtime does)
    psvrecore_generated_code(psvrecore_codegen_device_tests)
endif()
