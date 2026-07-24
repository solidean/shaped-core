cmake_minimum_required(VERSION 3.28)
project(stb LANGUAGES C)

# Vendored stb single-file libraries (see vendor-stb.py for the pinned commit).
# We build a single static target from one impl TU; src/stb.c defines the STB_*_IMPLEMENTATION macros and `#include`s the headers, resolved via include/ below.
set(STB_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(stb STATIC ${STB_DIR}/src/stb.c)

# SYSTEM so the stb headers do not trip our warnings in the consuming TU.
target_include_directories(stb SYSTEM PUBLIC ${STB_DIR}/include)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(stb PRIVATE /W0)
else()
    target_compile_options(stb PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET stb PROPERTY POSITION_INDEPENDENT_CODE ON)
