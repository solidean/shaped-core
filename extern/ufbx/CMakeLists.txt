cmake_minimum_required(VERSION 3.28)
project(ufbx LANGUAGES C)

# Vendored ufbx (see vendor-ufbx.py for the pinned version).
# One TU: upstream is ufbx.c and ufbx.h and nothing else.
set(UFBX_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(ufbx STATIC
    ${UFBX_DIR}/src/ufbx.c
)

# SYSTEM so ufbx.h does not trip our warnings in the consuming TU.
target_include_directories(ufbx SYSTEM PUBLIC ${UFBX_DIR}/include)

# ufbx.c reaches ufbx.h with a quote include, which misses in src/ and resolves against include/ above.
target_include_directories(ufbx PRIVATE ${UFBX_DIR}/src)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(ufbx PRIVATE /W0)
else()
    target_compile_options(ufbx PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET ufbx PROPERTY POSITION_INDEPENDENT_CODE ON)
