# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

# Pico SDK: Umgebungsvariable PICO_SDK_PATH setzen oder die VS-Code-Pico-Extension nutzen
include(pico_sdk_import.cmake)

project(kc87_logger C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(kc87_logger main.c)
pico_generate_pio_header(kc87_logger ${CMAKE_CURRENT_LIST_DIR}/pulse_timer.pio)
pico_generate_pio_header(kc87_logger ${CMAKE_CURRENT_LIST_DIR}/pulse_out.pio)

target_link_libraries(kc87_logger pico_stdlib hardware_pio hardware_dma hardware_clocks)

pico_enable_stdio_usb(kc87_logger 1)
pico_enable_stdio_uart(kc87_logger 0)
pico_add_extra_outputs(kc87_logger)

# Test-Firmware: Rechtecksignal an GP3 zum Einstellen des PLAY-Pegels
add_executable(kc87_play_test play_test.c)

target_link_libraries(kc87_play_test pico_stdlib hardware_pwm hardware_clocks)

pico_enable_stdio_usb(kc87_play_test 1)
pico_enable_stdio_uart(kc87_play_test 0)
pico_add_extra_outputs(kc87_play_test)
