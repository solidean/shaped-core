cmake_minimum_required(VERSION 3.28)
project(shaped-graphics-webgpu LANGUAGES CXX)

# The WebGPU backend is its own static library over the shaped-graphics core.
# The enclosing CMakeLists adds it only under Emscripten with SC_WASM_WEBGPU, whose emdawnwebgpu port supplies <webgpu/webgpu.h>.
add_library(shaped-graphics-webgpu
    src/shaped-graphics/backends/webgpu/webgpu_binding.cc
    src/shaped-graphics/backends/webgpu/webgpu_command_list.cc
    src/shaped-graphics/backends/webgpu/webgpu_command_list.compute.cc
    src/shaped-graphics/backends/webgpu/webgpu_command_list.raster.cc
    src/shaped-graphics/backends/webgpu/webgpu_context.cc
    src/shaped-graphics/backends/webgpu/webgpu_context.create.cc
    src/shaped-graphics/backends/webgpu/webgpu_epoch.cc
    src/shaped-graphics/backends/webgpu/webgpu_format.cc
    src/shaped-graphics/backends/webgpu/webgpu_pipeline.cc
    src/shaped-graphics/backends/webgpu/webgpu_query.cc
    src/shaped-graphics/backends/webgpu/webgpu_resources.cc
    src/shaped-graphics/backends/webgpu/webgpu_stream.cc
    src/shaped-graphics/backends/webgpu/webgpu_swapchain.cc
    src/shaped-graphics/backends/webgpu/webgpu_transfer.cc
)

target_sources(shaped-graphics-webgpu
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-graphics/backends/webgpu/fwd.hh
    src/shaped-graphics/backends/webgpu/webgpu_binding_group.hh
    src/shaped-graphics/backends/webgpu/webgpu_binding_group_layout.hh
    src/shaped-graphics/backends/webgpu/webgpu_buffer.hh
    src/shaped-graphics/backends/webgpu/webgpu_command_list.hh
    src/shaped-graphics/backends/webgpu/webgpu_common.hh
    src/shaped-graphics/backends/webgpu/webgpu_constant_pages.hh
    src/shaped-graphics/backends/webgpu/webgpu_context.hh
    src/shaped-graphics/backends/webgpu/webgpu_format.hh
    src/shaped-graphics/backends/webgpu/webgpu_memory_heap.hh
    src/shaped-graphics/backends/webgpu/webgpu_pipeline.hh
    src/shaped-graphics/backends/webgpu/webgpu_query.hh
    src/shaped-graphics/backends/webgpu/webgpu_readback.hh
    src/shaped-graphics/backends/webgpu/webgpu_sampler.hh
    src/shaped-graphics/backends/webgpu/webgpu_stream.hh
    src/shaped-graphics/backends/webgpu/webgpu_swapchain.hh
    src/shaped-graphics/backends/webgpu/webgpu_texture.hh
    src/shaped-graphics/backends/webgpu/webgpu_upload_ring.hh
)

target_compile_features(shaped-graphics-webgpu PUBLIC cxx_std_23)

target_include_directories(shaped-graphics-webgpu
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(shaped-graphics-webgpu PUBLIC shaped-graphics)

sc_target_pch(shaped-graphics-webgpu SG)

# Backend-specific test binary: compute, raster, inline constants, samplers, texture copies and presentation, over hand-written WGSL.
if(SC_BUILD_TESTS)
    add_executable(shaped-graphics-webgpu-test
        tests/main.cc
        tests/webgpu-entry.cc
        tests/webgpu-compute-test.cc
        tests/webgpu-binding-test.cc
        tests/webgpu-raster-test.cc
        tests/webgpu-threading-test.cc
        tests/webgpu-stream-test.cc
    )
    sc_nexus_binary(shaped-graphics-webgpu-test KINDS tests)
    target_link_libraries(shaped-graphics-webgpu-test PRIVATE shaped-graphics-webgpu nexus)
endif()
