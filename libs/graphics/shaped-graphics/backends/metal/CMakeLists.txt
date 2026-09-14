cmake_minimum_required(VERSION 3.28)
project(shaped-graphics-metal LANGUAGES CXX)

# The metal backend is its own static library over the shaped-graphics core.
# The enclosing CMakeLists only adds this subdirectory on APPLE with the metal-cpp target present, so both are
# guaranteed available here.
#
# Everything is plain C++ over metal-cpp rather than Objective-C++, which is what keeps the repo's format, lint and PCH
# tooling — all of which speak only ('.cc', '.hh') — working unchanged. See extern/metal-cpp/dependency.yml.
add_library(shaped-graphics-metal
    src/shaped-graphics/backends/metal/metal_barrier.cc
    src/shaped-graphics/backends/metal/metal_buffer.cc
    src/shaped-graphics/backends/metal/metal_common.cc
    src/shaped-graphics/backends/metal/metal_command_list.cc
    src/shaped-graphics/backends/metal/metal_context.cc
    src/shaped-graphics/backends/metal/metal_context.create.cc
    src/shaped-graphics/backends/metal/metal_epoch.cc
    src/shaped-graphics/backends/metal/metal_feedback.cc
    src/shaped-graphics/backends/metal/metal_memory_heap.cc
    src/shaped-graphics/backends/metal/metal_residency.cc
    src/shaped-graphics/backends/metal/metal_staging_ring.cc
)

target_sources(shaped-graphics-metal
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-graphics/backends/metal/fwd.hh
    src/shaped-graphics/backends/metal/metal_barrier.hh
    src/shaped-graphics/backends/metal/metal_buffer.hh
    src/shaped-graphics/backends/metal/metal_buffer_access.hh
    src/shaped-graphics/backends/metal/metal_common.hh
    src/shaped-graphics/backends/metal/metal_command_list.hh
    src/shaped-graphics/backends/metal/metal_context.hh
    src/shaped-graphics/backends/metal/metal_epoch.hh
    src/shaped-graphics/backends/metal/metal_feedback.hh
    src/shaped-graphics/backends/metal/metal_memory_heap.hh
    src/shaped-graphics/backends/metal/metal_residency.hh
    src/shaped-graphics/backends/metal/metal_staging_ring.hh
)

target_compile_features(shaped-graphics-metal PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-graphics/backends/metal/...>' works for consumers.
target_include_directories(shaped-graphics-metal
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# metal-cpp carries the headers plus the Metal / Foundation / QuartzCore framework links.
target_link_libraries(shaped-graphics-metal PUBLIC shaped-graphics metal-cpp)

# SG rather than a metal-specific tier: the SG tier holds no Metal header, and a tier over <Metal/Metal.hpp> would be
# worth measuring before it is worth having.
sc_target_pch(shaped-graphics-metal SG)

# Backend-specific test binary: device creation, the floor refusal, epochs and command-list lifetime.
# Named *-test so the dev.py runner discovers it.
if(SC_BUILD_TESTS)
    add_executable(shaped-graphics-metal-test
        tests/main.cc
        tests/metal-barrier-test.cc
        tests/metal-test.cc
        tests/metal-transfer-test.cc
        tests/metal-validation-test.cc
    )

    target_link_libraries(shaped-graphics-metal-test PRIVATE shaped-graphics-metal nexus)
endif()
