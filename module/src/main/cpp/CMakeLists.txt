cmake_minimum_required(VERSION 3.22.1)
# Compatible with NDK 29.0.14206865
project(sample)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(LINKER_FLAGS "-ffixed-x18 -Wl,--hash-style=both")

set(CXX_FLAGS "${CXX_FLAGS} -fno-exceptions -fno-rtti -fvisibility=hidden -fvisibility-inlines-hidden")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CXX_FLAGS}")

set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LINKER_FLAGS}")
set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${LINKER_FLAGS}")

add_library(${MODULE_NAME} SHARED
    device_spoof.cpp
    ConfigManager.cpp
    HookManager.cpp
    external/simdjson/simdjson.cpp
)
target_include_directories(${MODULE_NAME} PRIVATE
    external/simdjson)
target_link_libraries(${MODULE_NAME} log)
