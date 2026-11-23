# A program built against the installed STXlib (run.sh).
cmake_minimum_required(VERSION 3.24)
project(stxlib_consumer CXX)
set(CMAKE_CXX_STANDARD 20)
find_package(stxlib CONFIG REQUIRED)
add_executable(stxlib_consumer main.cpp)
target_link_libraries(stxlib_consumer PRIVATE stxlib::algorithms stxlib::boundary stxlib::state)
set_target_properties(stxlib_consumer PROPERTIES BUILD_RPATH "${stxlib_DIR}/../..")
