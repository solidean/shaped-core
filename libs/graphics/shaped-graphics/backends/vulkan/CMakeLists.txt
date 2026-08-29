cmake_minimum_required(VERSION 3.28)
project(shaped-graphics-vulkan LANGUAGES CXX)

# The vulkan backend is its own static library over the shaped-graphics core.
# The enclosing CMakeLists only adds this subdirectory when find_package(Vulkan) succeeds, so Vulkan::Vulkan is guaranteed available here.
add_library(shaped-graphics-vulkan
    src/shaped-graphics/backends/vulkan/vulkan_context.cc
    src/shaped-graphics/backends/vulkan/vulkan_context.create.cc
    src/shaped-graphics/backends/vulkan/vulkan_epoch.cc
    src/shaped-graphics/backends/vulkan/vulkan_command_list.cc
    src/shaped-graphics/backends/vulkan/vulkan_barrier.cc
    src/shaped-graphics/backends/vulkan/vulkan_buffer.cc
    src/shaped-graphics/backends/vulkan/vulkan_format.cc
    src/shaped-graphics/backends/vulkan/vulkan_texture.cc
)

target_sources(shaped-graphics-vulkan
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-graphics/backends/vulkan/fwd.hh
    src/shaped-graphics/backends/vulkan/vulkan_common.hh
    src/shaped-graphics/backends/vulkan/vulkan_context.hh
    src/shaped-graphics/backends/vulkan/vulkan_epoch.hh
    src/shaped-graphics/backends/vulkan/vulkan_command_list.hh
    src/shaped-graphics/backends/vulkan/vulkan_barrier.hh
    src/shaped-graphics/backends/vulkan/vulkan_buffer.hh
    src/shaped-graphics/backends/vulkan/vulkan_format.hh
    src/shaped-graphics/backends/vulkan/vulkan_texture.hh
)

target_compile_features(shaped-graphics-vulkan PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-graphics/backends/vulkan/...>' works for consumers.
target_include_directories(shaped-graphics-vulkan
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Vulkan::Vulkan carries the loader link plus the SDK include dir (for <vulkan/vulkan.h>).
target_link_libraries(shaped-graphics-vulkan PUBLIC shaped-graphics Vulkan::Vulkan)

# SG rather than a vulkan-specific tier: <vulkan/vulkan.h> reaches an UNSANITIZED windows.h under
# VK_USE_PLATFORM_WIN32_KHR, which is the collision dx12_common.hh's bracket exists to prevent, and 6 TUs do not justify the risk.
sc_target_pch(shaped-graphics-vulkan SG)

target_compile_options(shaped-graphics-vulkan
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Backend-specific test binary: device creation, command lists, buffers, epochs.
# Named *-test so the dev.py runner discovers it.
if(SC_BUILD_TESTS)
    add_executable(shaped-graphics-vulkan-test
        tests/main.cc
        tests/vulkan-barrier-test.cc
        tests/vulkan-test.cc
    )

    target_link_libraries(shaped-graphics-vulkan-test PRIVATE shaped-graphics-vulkan nexus)
endif()
