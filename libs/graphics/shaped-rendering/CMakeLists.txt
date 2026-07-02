cmake_minimum_required(VERSION 3.28)
project(shaped-rendering LANGUAGES CXX)

# shaped-rendering (sr::) builds render routines and helpers on top of shaped-graphics (mipmap
# generation, tonemapping, texture compression, ...). Early-stage skeleton: a single anchor
# translation unit keeps the static target well-formed until routines land.
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

target_link_libraries(shaped-rendering PUBLIC shaped-graphics)

target_compile_options(shaped-rendering
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Test executable
add_executable(shaped-rendering-test
    tests/main.cc
    tests/smoke-test.cc
)

target_link_libraries(shaped-rendering-test
    PRIVATE
    shaped-rendering
    nexus
)

# Browser test runner (Emscripten only); a no-op on native toolchains.
sc_add_nexus_web_runner(shaped-rendering-test)
