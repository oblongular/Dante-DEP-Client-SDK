cmake_minimum_required(VERSION 3.16)
project(dep_sync_fanoutd CXX)

add_executable(dep_sync_fanoutd dep_sync_fanoutd.cpp)

target_include_directories(dep_sync_fanoutd PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)

set_property(TARGET dep_sync_fanoutd PROPERTY CXX_STANDARD 17)
set_property(TARGET dep_sync_fanoutd PROPERTY CXX_STANDARD_REQUIRED ON)

# sem_* is in libpthread; clock_gettime in librt on older glibc (noop on glibc ≥ 2.17)
target_link_libraries(dep_sync_fanoutd PRIVATE pthread rt)

install(TARGETS dep_sync_fanoutd DESTINATION bin)
install(FILES dep_sync_fanoutd.service DESTINATION lib/systemd/system)
