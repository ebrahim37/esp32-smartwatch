cmake_minimum_required(VERSION 3.16)

# Shared protocol library + watch-local components.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../shared/protocol")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(smartwatch)
