cmake_minimum_required(VERSION 3.13)

project(kc87_tools C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(serial_capture serial_capture.c)
add_executable(serial_transmit serial_transmit.c)
