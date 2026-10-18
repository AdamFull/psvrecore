add_executable(psvinfo main.cpp)
target_link_libraries(psvinfo PRIVATE psvrecore::formats)
target_compile_definitions(psvinfo PRIVATE PSVRECORE_NID_DB_DIR="${PSVRECORE_NID_DB_DIR}")
psvrecore_target_defaults(psvinfo)
