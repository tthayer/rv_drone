cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

# Needs -DPICO_SDK_PATH=... or PICO_SDK_PATH in the environment
# (tools/get-pico-sdk.sh puts it in third_party/pico-sdk).
include(pico_sdk_import.cmake)

project(rv_drone_pico C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_subdirectory(common)
add_subdirectory(audio)   # Pico A
# add_subdirectory(panel) # Pico B (M3)
