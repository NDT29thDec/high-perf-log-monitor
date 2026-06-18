cmake_minimum_required(VERSION 3.20)
project(LogMonitor
    VERSION   1.0.0
    LANGUAGES CXX
    DESCRIPTION "Real-time System Log Parser & Monitor"
)

# ==============================================================================
# Global settings
# ==============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Compiler-specific warning flags
if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion)
endif()

# ==============================================================================
# Threading
# ==============================================================================
find_package(Threads REQUIRED)

# ==============================================================================
# Subdirectories
# ==============================================================================
add_subdirectory(src)
add_subdirectory(app)

option(BUILD_TESTS "Build unit tests" ON)
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
