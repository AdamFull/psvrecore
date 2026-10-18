add_executable(psvdis main.cpp)
target_link_libraries(psvdis PRIVATE psvrecore::arm)
psvrecore_target_defaults(psvdis)
