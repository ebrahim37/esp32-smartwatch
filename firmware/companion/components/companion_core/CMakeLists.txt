# Hardware-independent companion logic (storage policy, quotas, sync planning, auth helpers).
# Compiles for ESP-IDF and for host unit tests (tests/host).
set(CCORE_SRCS src/sd_policy.cpp src/quota.cpp src/sync_plan.cpp src/md5.cpp src/subsonic.cpp src/backoff.cpp src/play_queue.cpp src/pcm.cpp src/ntfy.cpp)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${CCORE_SRCS} INCLUDE_DIRS include REQUIRES protocol)
    return()
endif()
add_library(companion_core STATIC ${CCORE_SRCS})
target_include_directories(companion_core PUBLIC include)
target_compile_features(companion_core PUBLIC cxx_std_17)
