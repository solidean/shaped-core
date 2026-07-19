cmake_minimum_required(VERSION 3.28)
project(shaped-rendering LANGUAGES CXX)

# shaped-rendering (sr::) builds concrete render routines and helpers on top of shaped-graphics (mipmap
# generation, tonemapping, texture compression, ...). The render-routine *framework* (the routine base
# and the per-context ctx.routines registry) lives in shaped-graphics; sr hosts the concrete routines,
# which land in their own .cc files as they are implemented.
add_library(shaped-rendering
    src/shaped-rendering/shaped-rendering.cc
)

target_sources(shaped-rendering
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-rendering/fwd.hh
    src/shaped-rendering/all.hh
)

target_compile_features(shaped-rendering PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-rendering/...>' works for consumers.
target_include_directories(shaped-rendering
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# sr depends on slib because concrete routines acquire their shaders through it (a routine-author
# dependency). The routine framework in sg needs no slib — reload tracking is sg's own generation
# counter. slib is added after us in the root CMakeLists, which is fine — target_link_libraries
# resolves item names at generate time (shaped-graphics-test does the same).
target_link_libraries(shaped-rendering PUBLIC shaped-graphics shaped-shader-library)

target_compile_options(shaped-rendering
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# No test target yet: sr currently hosts no concrete routines, and the render-routine *framework* is
# tested in shaped-graphics (tests/routine/), where it lives. A shaped-rendering-test returns as the
# first concrete routine lands, with its own tests.
