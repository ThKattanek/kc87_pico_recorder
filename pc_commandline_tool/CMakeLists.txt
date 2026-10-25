cmake_minimum_required(VERSION 3.13)

project(kc87tool CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

add_executable(kc87tool kc87tool.cpp)
target_link_libraries(kc87tool PRIVATE Threads::Threads)

if(MSVC)
    target_compile_options(kc87tool PRIVATE /W3 /EHsc)
else()
    target_compile_options(kc87tool PRIVATE -Wall -Wextra)
endif()
