cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../shared/protocol")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Only build components that are actually required.
idf_build_set_property(MINIMAL_BUILD ON)
project(companion)
