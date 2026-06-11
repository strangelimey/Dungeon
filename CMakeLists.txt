cmake_minimum_required(VERSION 3.24)
project(Dungeon LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Windows-wide defines
add_compile_definitions(
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

if(MSVC)
    add_compile_options(/W4 /EHsc /MP /utf-8)
    # Treat the most dangerous warnings as errors without being draconian.
    add_compile_options(/we4715) # not all control paths return a value
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Third-party single-header libraries (interface target + one impl TU)
add_subdirectory(external)

# Engine modules — order matches the dependency graph (see docs/ARCHITECTURE.md)
add_subdirectory(src/Core)
add_subdirectory(src/Platform)
add_subdirectory(src/Assets)
add_subdirectory(src/Animation)
add_subdirectory(src/Graphics)
add_subdirectory(src/UI)
add_subdirectory(src/Audio)
add_subdirectory(src/Game)
add_subdirectory(src/Main)

# Offline tooling (not part of the game).
add_subdirectory(tools/AssetBaker)

# Visual Studio solution: open with the game as the startup project.
set_property(DIRECTORY ${CMAKE_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Dungeon)
