cmake_minimum_required(VERSION 3.13)

project(serial_capture C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(serial_capture serial_capture.c)
