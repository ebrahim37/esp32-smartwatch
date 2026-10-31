cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../shared/protocol")
# Only build components that are actually required (keeps the 4 MB image lean and builds fast).
set(MINIMAL_BUILD ON)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(companion)
