cmake_minimum_required(VERSION 3.24)

project(wasafe_vcd_test_package LANGUAGES CXX)

find_package(wasafe_vcd CONFIG REQUIRED)

add_executable(example src/example.cpp)
target_link_libraries(example PRIVATE wasafe::wasafe_vcd)
