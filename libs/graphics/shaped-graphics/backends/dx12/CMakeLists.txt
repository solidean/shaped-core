cmake_minimum_required(VERSION 3.28)
project(shaped-graphics-dx12 LANGUAGES CXX)

# The dx12 backend is its own static library that links the shaped-graphics core. It is
# currently a pure stub (every entry point is CC_UNREACHABLE) with no DirectX 12 SDK
# dependency, so it compiles anywhere the enclosing CMakeLists enables it (Windows only). Real
# SDK detection/linking lands with the first buffer upload/download/copy milestone.
add_library(shaped-graphics-dx12
    src/shaped-graphics/backends/dx12/dx12.cc
)

target_sources(shaped-graphics-dx12
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-graphics/backends/dx12/fwd.hh
    src/shaped-graphics/backends/dx12/dx12_context.hh
    src/shaped-graphics/backends/dx12/dx12_command_list.hh
    src/shaped-graphics/backends/dx12/dx12_buffer.hh
)

target_compile_features(shaped-graphics-dx12 PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-graphics/backends/dx12/...>' works for consumers.
target_include_directories(shaped-graphics-dx12
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(shaped-graphics-dx12 PUBLIC shaped-graphics)

target_compile_options(shaped-graphics-dx12
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)
