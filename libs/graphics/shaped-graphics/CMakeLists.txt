cmake_minimum_required(VERSION 3.28)
project(shaped-graphics LANGUAGES CXX)

# shaped-graphics (sg::) is the backend-agnostic graphics-API core: context / command_list /
# GPU resource types plus the pure-virtual backend bridge. Concrete backends live under
# backends/ as their own static libraries and are added conditionally below.
add_library(shaped-graphics
    src/shaped-graphics/shaped-graphics.cc
    src/shaped-graphics/buffer.cc
    src/shaped-graphics/command_list.cc
    src/shaped-graphics/context.cc
)

# Public headers live co-located in src/. FILE_SET models the public API explicitly.
target_sources(shaped-graphics
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-graphics/fwd.hh
    src/shaped-graphics/all.hh
    src/shaped-graphics/types.hh
    src/shaped-graphics/context.hh
    src/shaped-graphics/command_list.hh
    src/shaped-graphics/buffer.hh
)

target_compile_features(shaped-graphics PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-graphics/...>' works for consumers.
target_include_directories(shaped-graphics
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# clean-core provides the vocabulary types + assertions; typed-geometry the math types sg's
# resource/viewport/geometry APIs will speak in.
target_link_libraries(shaped-graphics PUBLIC clean-core typed-geometry)

# Enable conforming preprocessor for MSVC
target_compile_options(shaped-graphics
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Backends: each is its own static library that links this core. A backend is built only where
# it is available for the platform/build; the stubs carry no SDK dependency yet, so the gates
# are platform-only for now (dx12 -> Windows, vulkan -> native desktop). Enabled backends are
# collected so the test binary link-checks them.
set(_sg_backends "")

if(WIN32)
    add_subdirectory(backends/dx12)
    list(APPEND _sg_backends shaped-graphics-dx12)
endif()

if(NOT EMSCRIPTEN)
    add_subdirectory(backends/vulkan)
    list(APPEND _sg_backends shaped-graphics-vulkan)
endif()

# Test executable. This binary stays backend-agnostic (the abstract sg surface only). Backend-
# specific tests live with their backend as their own *-test binary (see backends/dx12/tests), so
# they are naturally guarded to the platform where that backend builds.
add_executable(shaped-graphics-test
    tests/main.cc
    tests/smoke-test.cc
)

target_link_libraries(shaped-graphics-test
    PRIVATE
    shaped-graphics
    ${_sg_backends}
    nexus
)

# Browser test runner (Emscripten only); a no-op on native toolchains.
sc_add_nexus_web_runner(shaped-graphics-test)
