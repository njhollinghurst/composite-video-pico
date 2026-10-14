cmake_minimum_required(VERSION 3.12)

# initialize the SDK based on PICO_SDK_PATH
# note: this must happen before project()
include(pico_sdk_import.cmake)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

project(composite-video-pico C CXX ASM)

# initialize the Pico SDK
pico_sdk_init()

# rest of your project
add_subdirectory(minimal)
