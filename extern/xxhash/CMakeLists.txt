cmake_minimum_required(VERSION 3.28)
project(xxhash LANGUAGES C)

# Vendored xxHash (see vendor-xxhash.py for the pinned version). We build a
# single static target from upstream's single-TU entry point; src/xxhash.c only
# `#include "xxhash.h"`, resolved via the include/ dir below.
set(XXHASH_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(xxhash STATIC ${XXHASH_DIR}/src/xxhash.c)

# SYSTEM so xxhash.h does not trip our warnings in the consuming TU.
target_include_directories(xxhash SYSTEM PUBLIC ${XXHASH_DIR}/include)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(xxhash PRIVATE /W0)
else()
    target_compile_options(xxhash PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET xxhash PROPERTY POSITION_INDEPENDENT_CODE ON)
