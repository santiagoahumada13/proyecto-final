cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico_w)

include(pico_sdk_import.cmake)

include(FreeRTOS_Kernel_import.cmake)

project(proyectofinal)

pico_sdk_init()

add_subdirectory(src)

#add_subdirectory(lib)