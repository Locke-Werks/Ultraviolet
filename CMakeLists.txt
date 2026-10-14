cmake_minimum_required(VERSION 3.24)

project(Ultraviolet
    VERSION 0.1.0
    DESCRIPTION "Audio upscaler"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
endif()

enable_testing()

add_subdirectory(third_party)
add_subdirectory(src/core)
add_subdirectory(src/app)
add_subdirectory(tests)
