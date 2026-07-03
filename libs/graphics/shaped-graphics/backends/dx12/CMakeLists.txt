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
    src/shaped-graphics/backends/dx12/dx12_buffer.cc
    src/shaped-graphics/backends/dx12/dx12_upload_inline.cc
    src/shaped-graphics/backends/dx12/dx12_download_inline.cc
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
    src/shaped-graphics/backends/dx12/dx12_buffer.hh
    src/shaped-graphics/backends/dx12/dx12_resource_upload.hh
    src/shaped-graphics/backends/dx12/dx12_resource_download.hh
    src/shaped-graphics/backends/dx12/dx12_upload_inline.hh
    src/shaped-graphics/backends/dx12/dx12_download_inline.hh
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

# Backend-specific test binary. Small, but this is the home for real dx12-specific tests (device
# creation, command lists, buffers). Named *-test so the dev.py runner discovers it. It only exists
# on Windows because this whole subdirectory is gated there by the sg CMakeLists.
add_executable(shaped-graphics-dx12-test
    tests/main.cc
    tests/dx12-test.cc
)

target_link_libraries(shaped-graphics-dx12-test PRIVATE shaped-graphics-dx12 nexus)
