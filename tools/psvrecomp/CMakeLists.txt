add_executable(psvrecomp main.cpp)
target_link_libraries(psvrecomp PRIVATE psvrecore::analysis psvrecore::recomp)
target_compile_definitions(psvrecomp PRIVATE PSVRECORE_NID_DB_DIR="${PSVRECORE_NID_DB_DIR}")
psvrecore_target_defaults(psvrecomp)
