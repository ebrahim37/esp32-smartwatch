# Host-side unit tests for all hardware-independent firmware logic.
# Run via scripts/sw test (inside `nix develop`).
cmake_minimum_required(VERSION 3.16)
project(smartwatch_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer)
add_link_options(-fsanitize=address,undefined)

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(SWP_NO_TESTS ON)
add_subdirectory(${ROOT}/shared/protocol swp)
add_subdirectory(${ROOT}/firmware/watch/components/watch_core watch_core)
add_subdirectory(${ROOT}/firmware/companion/components/companion_core companion_core)

enable_testing()
include_directories(${ROOT}/shared/protocol/test)

add_executable(protocol_tests ${ROOT}/shared/protocol/test/test_protocol.cpp)
target_link_libraries(protocol_tests PRIVATE swp)
add_test(NAME protocol COMMAND protocol_tests)

foreach(core watch_core companion_core)
    add_executable(${core}_tests test_${core}.cpp)
    target_link_libraries(${core}_tests PRIVATE ${core})
    add_test(NAME ${core} COMMAND ${core}_tests)
endforeach()

find_package(Threads REQUIRED)
add_executable(fetch_tests test_fetch.cpp ${ROOT}/firmware/watch/main/ble/fetch.cpp)
target_include_directories(fetch_tests PRIVATE stubs ${ROOT}/firmware/watch/main)
target_link_libraries(fetch_tests PRIVATE swp Threads::Threads)
add_test(NAME fetch COMMAND fetch_tests)
