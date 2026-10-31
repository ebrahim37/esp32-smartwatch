# Hardware-independent watch logic. Compiles for ESP-IDF and for host unit tests
# (tests/host, see scripts/sw test).
set(WATCH_CORE_SRCS
    src/step_counter.cpp src/heading.cpp src/calculator.cpp src/alarm_schedule.cpp
    src/format.cpp src/notification_store.cpp src/map_math.cpp src/swt.cpp)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${WATCH_CORE_SRCS} INCLUDE_DIRS include REQUIRES protocol)
    return()
endif()
add_library(watch_core STATIC ${WATCH_CORE_SRCS})
target_include_directories(watch_core PUBLIC include)
target_compile_features(watch_core PUBLIC cxx_std_17)
