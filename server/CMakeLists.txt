# The signaling server. A standalone exe, and deliberately NOT part of mgmp.dll:
# it is a separate process on a separate machine most of the time, it must keep
# running across game restarts, and nothing in it belongs in the game's address
# space.
#
# It shares exactly one thing with the mod -- third_party/json.hpp -- so the
# protocol's two ends are written against the same JSON implementation.
#
# ONE SOURCE, TWO PLATFORMS: mgmp_server.cpp builds on Windows (MSVC) and Linux (g++/clang). Built from the main
# project this file is included by its CMakeLists; built on its own (the Linux case) it is a project of its own:
#     cd server && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build   ->  build/mgmp_server
# (or simply linux/build.sh).
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.16)
    project(mgmp_server CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
        set(CMAKE_BUILD_TYPE Release)
    endif()
endif()

add_executable(mgmp_server mgmp_server.cpp)

# third_party is where json.hpp lives, the one include it shares with the mod.
target_include_directories(mgmp_server PRIVATE ${CMAKE_CURRENT_LIST_DIR}/../third_party)

if(WIN32)
    target_compile_definitions(mgmp_server PRIVATE WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS NOMINMAX)
    target_compile_options(mgmp_server PRIVATE /W3)
    target_link_libraries(mgmp_server PRIVATE ws2_32)
else()
    target_compile_options(mgmp_server PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
