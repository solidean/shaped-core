cmake_minimum_required(VERSION 3.28)
project(meshoptimizer LANGUAGES CXX)

# Vendored meshoptimizer decoders (see vendor-meshoptimizer.py for the pinned version and the subset).
# Three TUs: the vertex codec, the index codec and the vertex filters, each picking its SIMD path from the target.
set(MESHOPTIMIZER_DIR ${CMAKE_CURRENT_SOURCE_DIR})

add_library(meshoptimizer STATIC
    ${MESHOPTIMIZER_DIR}/src/vertexcodec.cpp
    ${MESHOPTIMIZER_DIR}/src/indexcodec.cpp
    ${MESHOPTIMIZER_DIR}/src/vertexfilter.cpp
)

# SYSTEM so meshoptimizer.h does not trip our warnings in the consuming TU.
target_include_directories(meshoptimizer SYSTEM PUBLIC ${MESHOPTIMIZER_DIR}/include)

# The sources reach meshoptimizer.h with a quote include, which misses in src/ and resolves against include/ above.
target_include_directories(meshoptimizer PRIVATE ${MESHOPTIMIZER_DIR}/src)

# Third-party code: silence its warnings, hide its symbols (statically absorbed).
if (MSVC)
    target_compile_options(meshoptimizer PRIVATE /W0)
else()
    target_compile_options(meshoptimizer PRIVATE -w -fvisibility=hidden)
endif()

set_property(TARGET meshoptimizer PROPERTY POSITION_INDEPENDENT_CODE ON)
