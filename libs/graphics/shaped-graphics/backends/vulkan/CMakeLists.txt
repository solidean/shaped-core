cmake_minimum_required(VERSION 3.28)
project(shaped-graphics-vulkan LANGUAGES CXX)

# The vulkan backend is its own static library that links the shaped-graphics core. It is
# currently a pure stub (every entry point is CC_UNREACHABLE) with no Vulkan SDK dependency, so
# it compiles anywhere the enclosing CMakeLists enables it (native desktop). Real SDK
# detection/linking lands with the first buffer upload/download/copy milestone.
add_library(shaped-graphics-vulkan
    src/shaped-graphics/backends/vulkan/vulkan_context.cc
    src/shaped-graphics/backends/vulkan/vulkan_command_list.cc
)

target_sources(shaped-graphics-vulkan
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-graphics/backends/vulkan/fwd.hh
    src/shaped-graphics/backends/vulkan/vulkan_context.hh
    src/shaped-graphics/backends/vulkan/vulkan_command_list.hh
)

target_compile_features(shaped-graphics-vulkan PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-graphics/backends/vulkan/...>' works for consumers.
target_include_directories(shaped-graphics-vulkan
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(shaped-graphics-vulkan PUBLIC shaped-graphics)

target_compile_options(shaped-graphics-vulkan
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)
