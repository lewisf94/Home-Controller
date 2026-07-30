cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico CACHE STRING "RP2040 board")

if(NOT DEFINED PICO_SDK_PATH)
    if(DEFINED ENV{PICO_SDK_PATH})
        set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
    else()
        message(FATAL_ERROR "PICO_SDK_PATH is not set. Run build.ps1.")
    endif()
endif()

include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")

project(smartknob_rp2040 C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(smartknob_rp2040
    src/main.cpp
    src/mt6701_i2c.cpp
)

add_executable(smartknob_motor_test
    src/motor_main.cpp
)

target_compile_options(smartknob_rp2040 PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_compile_options(smartknob_motor_test PRIVATE
    -Wall
    -Wextra
    -Werror
)

target_link_libraries(smartknob_rp2040 PRIVATE
    pico_stdlib
    hardware_clocks
    hardware_i2c
)

target_link_libraries(smartknob_motor_test PRIVATE
    pico_stdlib
    hardware_clocks
    hardware_pwm
)

pico_enable_stdio_usb(smartknob_rp2040 1)
pico_enable_stdio_uart(smartknob_rp2040 0)
pico_add_extra_outputs(smartknob_rp2040)

pico_enable_stdio_usb(smartknob_motor_test 1)
pico_enable_stdio_uart(smartknob_motor_test 0)
pico_add_extra_outputs(smartknob_motor_test)
