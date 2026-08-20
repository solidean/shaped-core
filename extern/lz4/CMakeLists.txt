cmake_minimum_required(VERSION 3.28)
project(lz4 LANGUAGES C)

# Vendored LZ4 (see vendor-lz4.py for the pinned version). Three TUs: the block
# codec, the high-compression codec, and the frame layer, plus the xxhash copy
# lz4frame checksums frames with.
set(LZ4_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(lz4 STATIC
    ${LZ4_DIR}/src/lz4.c
    ${LZ4_DIR}/src/lz4hc.c
    ${LZ4_DIR}/src/lz4frame.c
    ${LZ4_DIR}/src/xxhash.c
)

# SYSTEM so the lz4 headers do not trip our warnings in the consuming TU.
target_include_directories(lz4 SYSTEM PUBLIC ${LZ4_DIR}/include)

# lz4's own xxhash.h lives here rather than in include/, so it cannot shadow the separately vendored
# extern/xxhash/include/xxhash.h that cc::hash128 includes. Only lz4frame.c reaches it, via a quote include.
target_include_directories(lz4 PRIVATE ${LZ4_DIR}/src)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(lz4 PRIVATE /W0)
else()
    target_compile_options(lz4 PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET lz4 PROPERTY POSITION_INDEPENDENT_CODE ON)
