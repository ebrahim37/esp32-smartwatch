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
target_link_libraries(watch_core PUBLIC swp)
add_subdirectory(${ROOT}/firmware/companion/components/companion_core companion_core)
target_link_libraries(companion_core PUBLIC swp)

enable_testing()
include_directories(${ROOT}/shared/protocol/test)

add_executable(protocol_tests ${ROOT}/shared/protocol/test/test_protocol.cpp)
target_link_libraries(protocol_tests PRIVATE swp)
add_test(NAME protocol COMMAND protocol_tests)

add_executable(watch_core_tests test_watch_core.cpp)
target_link_libraries(watch_core_tests PRIVATE watch_core)
add_test(NAME watch_core COMMAND watch_core_tests)

add_executable(companion_core_tests test_companion_core.cpp)
target_link_libraries(companion_core_tests PRIVATE companion_core)
add_test(NAME companion_core COMMAND companion_core_tests)
