cmake_minimum_required(VERSION 3.28)
project(shaped-graphics-vulkan LANGUAGES CXX)

# The vulkan backend is its own static library over the shaped-graphics core.
# The enclosing CMakeLists only adds this subdirectory when find_package(Vulkan) succeeds, so Vulkan::Vulkan is guaranteed available here.
add_library(shaped-graphics-vulkan
    src/shaped-graphics/backends/vulkan/vulkan_context.cc
    src/shaped-graphics/backends/vulkan/vulkan_context.create.cc
    src/shaped-graphics/backends/vulkan/vulkan_download_inline.cc
    src/shaped-graphics/backends/vulkan/vulkan_descriptor_functions.cc
    src/shaped-graphics/backends/vulkan/vulkan_descriptor_heap.cc
    src/shaped-graphics/backends/vulkan/vulkan_epoch.cc
    src/shaped-graphics/backends/vulkan/vulkan_driver_lock.cc
    src/shaped-graphics/backends/vulkan/vulkan_completion_group.cc
    src/shaped-graphics/backends/vulkan/vulkan_query.cc
    src/shaped-graphics/backends/vulkan/vulkan_command_list.queries.cc
    src/shaped-graphics/backends/vulkan/vulkan_download_async.cc
    src/shaped-graphics/backends/vulkan/vulkan_upload_async.cc
    src/shaped-graphics/backends/vulkan/vulkan_command_list.cc
    src/shaped-graphics/backends/vulkan/vulkan_command_list.compute.cc
    src/shaped-graphics/backends/vulkan/vulkan_command_list.raster.cc
    src/shaped-graphics/backends/vulkan/vulkan_barrier.cc
    src/shaped-graphics/backends/vulkan/vulkan_binding_group.cc
    src/shaped-graphics/backends/vulkan/vulkan_compute_pipeline.cc
    src/shaped-graphics/backends/vulkan/vulkan_staging_binding_group.cc
    src/shaped-graphics/backends/vulkan/vulkan_binding_group_layout.cc
    src/shaped-graphics/backends/vulkan/vulkan_pipeline_layout.cc
    src/shaped-graphics/backends/vulkan/vulkan_raster_pipeline.cc
    src/shaped-graphics/backends/vulkan/vulkan_raytracing.cc
    src/shaped-graphics/backends/vulkan/vulkan_raytracing_functions.cc
    src/shaped-graphics/backends/vulkan/vulkan_raytracing_pipeline.cc
    src/shaped-graphics/backends/vulkan/vulkan_raytracing_shader_table.cc
    src/shaped-graphics/backends/vulkan/vulkan_swapchain.cc
    src/shaped-graphics/backends/vulkan/vulkan_raster_state.cc
    src/shaped-graphics/backends/vulkan/vulkan_buffer.cc
    src/shaped-graphics/backends/vulkan/vulkan_format.cc
    src/shaped-graphics/backends/vulkan/vulkan_memory_heap.cc
    src/shaped-graphics/backends/vulkan/vulkan_sampler.cc
    src/shaped-graphics/backends/vulkan/vulkan_texture.cc
    src/shaped-graphics/backends/vulkan/vulkan_view_desc.cc
    src/shaped-graphics/backends/vulkan/vulkan_upload_inline.cc
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
    src/shaped-graphics/backends/vulkan/vulkan_download_inline.hh
    src/shaped-graphics/backends/vulkan/vulkan_descriptor_functions.hh
    src/shaped-graphics/backends/vulkan/vulkan_descriptor_heap.hh
    src/shaped-graphics/backends/vulkan/vulkan_epoch.hh
    src/shaped-graphics/backends/vulkan/vulkan_driver_lock.hh
    src/shaped-graphics/backends/vulkan/vulkan_command_list.hh
    src/shaped-graphics/backends/vulkan/vulkan_barrier.hh
    src/shaped-graphics/backends/vulkan/vulkan_binding_group_layout.hh
    src/shaped-graphics/backends/vulkan/vulkan_pipeline_layout.hh
    src/shaped-graphics/backends/vulkan/vulkan_buffer.hh
    src/shaped-graphics/backends/vulkan/vulkan_buffer_access.hh
    src/shaped-graphics/backends/vulkan/vulkan_format.hh
    src/shaped-graphics/backends/vulkan/vulkan_memory_heap.hh
    src/shaped-graphics/backends/vulkan/vulkan_sampler.hh
    src/shaped-graphics/backends/vulkan/vulkan_texture.hh
    src/shaped-graphics/backends/vulkan/vulkan_texture_access.hh
    src/shaped-graphics/backends/vulkan/vulkan_view_desc.hh
    src/shaped-graphics/backends/vulkan/vulkan_upload_inline.hh
)

target_compile_features(shaped-graphics-vulkan PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-graphics/backends/vulkan/...>' works for consumers.
target_include_directories(shaped-graphics-vulkan
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# Vulkan::Vulkan carries the loader link plus the SDK include dir (for <vulkan/vulkan.h>).
target_link_libraries(shaped-graphics-vulkan PUBLIC shaped-graphics Vulkan::Vulkan)

# SG rather than a vulkan-specific tier: a tier over <vulkan/vulkan.h> would bake the surface defines into the PCH,
# and those differ per translation unit, so the two that create surfaces could not share it anyway.
# The windows.h vulkan_common.hh reaches under VK_USE_PLATFORM_WIN32_KHR is sanitized there, so the header itself is no longer the reason.
sc_target_pch(shaped-graphics-vulkan SG)

target_compile_options(shaped-graphics-vulkan
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# Which windowing systems a swapchain can create a surface for.
#
# A VK_USE_PLATFORM_* define makes vulkan.h declare that platform's surface call, and pulls in that system's own
# header — so it is only correct where the development headers are actually installed.
# Probed rather than assumed: a machine with the Vulkan SDK and no X11 headers is ordinary, and guessing there turns a
# missing package into a compile error deep in a system header.
#
# Scoped to the translation units that create surfaces, because a surface define changes what vulkan.h declares and
# what it includes — on Windows it reaches windows.h, which vulkan_common.hh then has to sanitize.
# Keeping that off the other thirty TUs is cheaper than paying for it everywhere.
#
# With none of them the windowed paths compile to a runtime error naming the platform, which is the honest answer —
# the backend still builds and still presents headlessly.
set(_sc_vulkan_surface_defines "")
if(WIN32)
    list(APPEND _sc_vulkan_surface_defines VK_USE_PLATFORM_WIN32_KHR)
elseif(NOT APPLE)
    include(CheckIncludeFile)
    check_include_file("X11/Xlib.h" SC_VULKAN_HAS_XLIB_HEADERS)
    check_include_file("xcb/xcb.h" SC_VULKAN_HAS_XCB_HEADERS)
    check_include_file("wayland-client.h" SC_VULKAN_HAS_WAYLAND_HEADERS)

    if(SC_VULKAN_HAS_XLIB_HEADERS)
        list(APPEND _sc_vulkan_surface_defines VK_USE_PLATFORM_XLIB_KHR)
    endif()
    if(SC_VULKAN_HAS_XCB_HEADERS)
        list(APPEND _sc_vulkan_surface_defines VK_USE_PLATFORM_XCB_KHR)
    endif()
    if(SC_VULKAN_HAS_WAYLAND_HEADERS)
        list(APPEND _sc_vulkan_surface_defines VK_USE_PLATFORM_WAYLAND_KHR)
    endif()

    if(NOT _sc_vulkan_surface_defines)
        message(STATUS
            "shaped-graphics-vulkan: no X11, XCB or wayland development headers — a windowed swapchain reports "
            "unsupported. Headless presentation is unaffected.")
    endif()
endif()

# These two also skip the PCH.
# It is built without the surface defines, and clang-cl warns (-Wclang-cl-pch) on every compile whose macro set
# differs from the one the PCH was built with.
# The SG tier holds no vulkan header, so nothing in it actually varies with the define and the warning is noise —
# but it is noise on every build, and two TUs compiling without a PCH is the cheaper side of that trade.
if(_sc_vulkan_surface_defines)
    set_source_files_properties(
        src/shaped-graphics/backends/vulkan/vulkan_swapchain.cc
        src/shaped-graphics/backends/vulkan/vulkan_context.create.cc
        TARGET_DIRECTORY shaped-graphics-vulkan
        PROPERTIES
        COMPILE_DEFINITIONS "${_sc_vulkan_surface_defines}"
        SKIP_PRECOMPILE_HEADERS ON)
endif()

# Backend-specific test binary: device creation, command lists, buffers, epochs.
# Named *-test so the dev.py runner discovers it.
if(SC_BUILD_TESTS)
    add_executable(shaped-graphics-vulkan-test
        tests/main.cc
        tests/vulkan-barrier-test.cc
        tests/vulkan-buffer-access-test.cc
        tests/vulkan-concurrent-lists-test.cc
        tests/vulkan-compute-test.cc
        tests/vulkan-raster-test.cc
        tests/vulkan-query-test.cc
        tests/vulkan-raytracing-test.cc
        tests/vulkan-texture-access-test.cc
        tests/vulkan-texture-transfer-test.cc
        tests/vulkan-transient-attachment-test.cc
        tests/vulkan-test.cc
    )

    target_link_libraries(shaped-graphics-vulkan-test PRIVATE shaped-graphics-vulkan nexus)
endif()
