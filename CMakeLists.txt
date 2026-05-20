cmake_minimum_required(VERSION 3.20)

project(
    compiler
    LANGUAGES CXX
    DESCRIPTION "Exodus!"
    VERSION 0.0.1
)

if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Build speed toggles
option(ENABLE_CCACHE "Enable ccache for faster rebuilds" ON)

# Static Analysis Tools
option(ENABLE_CLANG_TIDY "Enable clang-tidy analysis during build" OFF)
option(ENABLE_IWYU "Enable Include-What-You-Use analysis during build" OFF)

if(ENABLE_CCACHE)
    find_program(CCACHE_EXE NAMES "ccache")
    if(CCACHE_EXE)
        message(STATUS "ccache found: ${CCACHE_EXE}")
        set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_EXE}")
    else()
        message(WARNING "ccache not found, disabling ENABLE_CCACHE")
    endif()
endif()

if(ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
    if(CLANG_TIDY_EXE)
        message(STATUS "clang-tidy found: ${CLANG_TIDY_EXE}")
        set(CMAKE_CXX_CLANG_TIDY "${CLANG_TIDY_EXE}")
    else()
        message(WARNING "clang-tidy not found, disabling ENABLE_CLANG_TIDY")
    endif()
endif()

if(ENABLE_IWYU)
    find_program(IWYU_EXE NAMES "include-what-you-use")
    if(IWYU_EXE)
        message(STATUS "iwyu found: ${IWYU_EXE}")
        set(CMAKE_CXX_INCLUDE_WHAT_YOU_USE "${IWYU_EXE}")
    else()
        message(WARNING "include-what-you-use not found, disabling ENABLE_IWYU")
    endif()
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_compile_definitions(__DEBUG)
endif()

find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

add_subdirectory(3rd-party/fmt)
add_subdirectory(include)
add_subdirectory(src)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
