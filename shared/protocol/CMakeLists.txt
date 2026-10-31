# Shared Smartwatch Protocol (SWP) library.
# Built as an ESP-IDF component by both firmware projects, or as a plain CMake library for
# host-side unit tests (see scripts/sw test).
if(ESP_PLATFORM)
    idf_component_register(
        SRCS src/wire.cpp src/frame.cpp src/transfer.cpp
        INCLUDE_DIRS include)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)
    return()
endif()

cmake_minimum_required(VERSION 3.16)
project(swp_protocol CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(swp STATIC src/wire.cpp src/frame.cpp src/transfer.cpp)
target_include_directories(swp PUBLIC include)
target_compile_options(swp PRIVATE -Wall -Wextra -Werror)

if(NOT SWP_NO_TESTS)
    enable_testing()
    add_executable(swp_tests test/test_protocol.cpp)
    target_link_libraries(swp_tests PRIVATE swp)
    target_compile_options(swp_tests PRIVATE -Wall -Wextra -fsanitize=address,undefined)
    target_link_options(swp_tests PRIVATE -fsanitize=address,undefined)
    add_test(NAME swp_tests COMMAND swp_tests)
endif()
