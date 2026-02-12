# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==

if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

set(FW_VERSION_MAJOR 0)
set(FW_VERSION_MINOR 1)
set(FW_VERSION_PATCH 0)
set(FW_VERSION_STRING "${FW_VERSION_MAJOR}.${FW_VERSION_MINOR}.${FW_VERSION_PATCH}")

# Pull in Raspberry Pi Pico SDK (must be before project)

include(pico_sdk_import.cmake)

project(kc87_pico_recorder C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Add executable. Default name is the project name, version 0.1

add_executable(kc87_pico_recorder kc87_pico_recorder.c )

target_compile_definitions(kc87_pico_recorder
        PRIVATE
        FW_VERSION_MAJOR=${FW_VERSION_MAJOR}
        FW_VERSION_MINOR=${FW_VERSION_MINOR}
        FW_VERSION_PATCH=${FW_VERSION_PATCH}
)

pico_set_program_name(kc87_pico_recorder "kc87_pico_recorder_fw")
pico_set_program_version(kc87_pico_recorder "${FW_VERSION_STRING}")

# Modify the below lines to enable/disable output over UART/USB
# UART0 (GPIO0/1) used for binary data stream
# USB used for debug printf output
pico_enable_stdio_uart(kc87_pico_recorder 0)
pico_enable_stdio_usb(kc87_pico_recorder 1)

# Add the standard library to the build
target_link_libraries(kc87_pico_recorder
        pico_stdlib hardware_gpio hardware_timer hardware_irq hardware_uart)

# Add the standard include files to the build
target_include_directories(kc87_pico_recorder PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
)

pico_add_extra_outputs(kc87_pico_recorder)

