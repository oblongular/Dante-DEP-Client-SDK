cmake_minimum_required(VERSION 3.16)
project(dep_wakeup_counter CXX)

add_executable(dep_wakeup_counter dep_wakeup_counter.cpp)

target_include_directories(dep_wakeup_counter PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../include"
)

set_property(TARGET dep_wakeup_counter PROPERTY CXX_STANDARD 17)
set_property(TARGET dep_wakeup_counter PROPERTY CXX_STANDARD_REQUIRED ON)

install(TARGETS dep_wakeup_counter DESTINATION bin)
