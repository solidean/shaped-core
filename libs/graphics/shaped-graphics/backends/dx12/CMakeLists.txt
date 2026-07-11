cmake_minimum_required(VERSION 3.28)
project(shaped-graphics-dx12 LANGUAGES CXX)

# The dx12 backend is its own static library that links the shaped-graphics core. It creates a
# live D3D12 device, command lists, and GPU buffers, so it links the DirectX 12 libs from the
# Windows SDK (always present on the Windows path the enclosing CMakeLists gates it to).
add_library(shaped-graphics-dx12
    src/shaped-graphics/backends/dx12/dx12_context.cc
    src/shaped-graphics/backends/dx12/dx12_context.create.cc
    src/shaped-graphics/backends/dx12/dx12_epoch.cc
    src/shaped-graphics/backends/dx12/dx12_command_list.cc
    src/shaped-graphics/backends/dx12/dx12_command_list.queries.cc
    src/shaped-graphics/backends/dx12/dx12_barrier.cc
    src/shaped-graphics/backends/dx12/dx12_command_allocator_pool.cc
    src/shaped-graphics/backends/dx12/dx12_buffer.cc
    src/shaped-graphics/backends/dx12/dx12_texture.cc
    src/shaped-graphics/backends/dx12/dx12_format.cc
    src/shaped-graphics/backends/dx12/dx12_memory_heap.cc
    src/shaped-graphics/backends/dx12/dx12_upload_inline.cc
    src/shaped-graphics/backends/dx12/dx12_download_inline.cc
    src/shaped-graphics/backends/dx12/dx12_upload_async.cc
    src/shaped-graphics/backends/dx12/dx12_download_async.cc
    src/shaped-graphics/backends/dx12/dx12_binding_group_layout.cc
    src/shaped-graphics/backends/dx12/dx12_pipeline_layout.cc
    src/shaped-graphics/backends/dx12/dx12_compute_pipeline.cc
    src/shaped-graphics/backends/dx12/dx12_raytracing_pipeline.cc
    src/shaped-graphics/backends/dx12/dx12_raytracing_shader_table.cc
    src/shaped-graphics/backends/dx12/dx12_binding_group.cc
    src/shaped-graphics/backends/dx12/dx12_descriptor_heap.cc
    src/shaped-graphics/backends/dx12/dx12_view_desc.cc
    src/shaped-graphics/backends/dx12/dx12_sampler.cc
    src/shaped-graphics/backends/dx12/dx12_texture_copy.cc
    src/shaped-graphics/backends/dx12/dx12_bind.cc
    src/shaped-graphics/backends/dx12/dx12_raytracing.cc
    src/shaped-graphics/backends/dx12/dx12_query.cc
)

target_sources(shaped-graphics-dx12
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-graphics/backends/dx12/fwd.hh
    src/shaped-graphics/backends/dx12/dx12_common.hh
    src/shaped-graphics/backends/dx12/dx12_context.hh
    src/shaped-graphics/backends/dx12/dx12_epoch.hh
    src/shaped-graphics/backends/dx12/dx12_command_list.hh
    src/shaped-graphics/backends/dx12/dx12_barrier.hh
    src/shaped-graphics/backends/dx12/dx12_command_allocator_pool.hh
    src/shaped-graphics/backends/dx12/dx12_buffer.hh
    src/shaped-graphics/backends/dx12/dx12_texture.hh
    src/shaped-graphics/backends/dx12/dx12_format.hh
    src/shaped-graphics/backends/dx12/dx12_memory_heap.hh
    src/shaped-graphics/backends/dx12/dx12_resource_upload.hh
    src/shaped-graphics/backends/dx12/dx12_resource_download.hh
    src/shaped-graphics/backends/dx12/dx12_upload_inline.hh
    src/shaped-graphics/backends/dx12/dx12_download_inline.hh
    src/shaped-graphics/backends/dx12/dx12_upload_async.hh
    src/shaped-graphics/backends/dx12/dx12_download_async.hh
    src/shaped-graphics/backends/dx12/dx12_descriptor_heap.hh
    src/shaped-graphics/backends/dx12/dx12_view_desc.hh
    src/shaped-graphics/backends/dx12/dx12_sampler.hh
    src/shaped-graphics/backends/dx12/dx12_texture_copy.hh
    src/shaped-graphics/backends/dx12/dx12_binding_group_layout.hh
    src/shaped-graphics/backends/dx12/dx12_pipeline_layout.hh
    src/shaped-graphics/backends/dx12/dx12_compute_pipeline.hh
    src/shaped-graphics/backends/dx12/dx12_raytracing_pipeline.hh
    src/shaped-graphics/backends/dx12/dx12_raytracing_shader_table.hh
    src/shaped-graphics/backends/dx12/dx12_binding_group.hh
    src/shaped-graphics/backends/dx12/dx12_acceleration_structure.hh
    src/shaped-graphics/backends/dx12/dx12_query.hh
)

target_compile_features(shaped-graphics-dx12 PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-graphics/backends/dx12/...>' works for consumers.
target_include_directories(shaped-graphics-dx12
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# d3d12/dxgi/dxguid ship with the Windows SDK on the default lib path; d3dcompiler is only needed
# once we compile shaders (not yet).
target_link_libraries(shaped-graphics-dx12 PUBLIC shaped-graphics d3d12 dxgi dxguid)

target_compile_options(shaped-graphics-dx12
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Backend-specific test binary. The home for real dx12-specific tests — feature smoke tests plus
# backend-internal invariant tests (pooling, epochs) that reach into concrete dx12 types. Split per
# topic; see libs/graphics/shaped-graphics/docs/concepts/backends.md. Named *-test so the dev.py
# runner discovers it. Only exists
# on Windows because this whole subdirectory is gated there by the sg CMakeLists.
add_executable(shaped-graphics-dx12-test
    tests/main.cc
    tests/dx12-context-test.cc
    tests/dx12-epoch-test.cc
    tests/dx12-command-pool-test.cc
    tests/dx12-transfer-test.cc
    tests/dx12-transfer-resize-test.cc
    tests/dx12-transfer-wrap-test.cc
    tests/dx12-upload-async-test.cc
    tests/dx12-download-async-test.cc
    tests/dx12-copy-test.cc
    tests/dx12-compute-test.cc
    tests/dx12-pipeline-cache-test.cc
    tests/dx12-cached-pso-test.cc
    tests/dx12-placed-test.cc
    tests/dx12-transient-test.cc
    tests/dx12-texture-barrier-test.cc
    tests/dx12-texture-view-test.cc
    tests/dx12-sampler-test.cc
    tests/dx12-texture-copy-test.cc
    tests/dx12-raytracing-test.cc
    tests/dx12-query-test.cc
)

target_link_libraries(shaped-graphics-dx12-test PRIVATE shaped-graphics-dx12 nexus)
