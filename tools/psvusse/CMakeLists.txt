add_executable(psvusse main.cpp)
target_link_libraries(psvusse PRIVATE psvrecore::usse)
psvrecore_target_defaults(psvusse)
