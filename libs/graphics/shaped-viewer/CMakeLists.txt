cmake_minimum_required(VERSION 3.28)
project(shaped-viewer LANGUAGES CXX)

# shaped-viewer (sv::) is the professional, RTX-enabled visualization renderer with a dev-friendly API,
# built on shaped-rendering. This is the first vertical slice: a per-frame viewer_definition of views, each
# path-traced into a target texture and blitted into a window by the view_renderer routine.
#
# The library API is present on every platform (the vocabulary, managers, routines and renderer all compile
# cross-platform); actual rendering needs a ray-tracing-capable backend, which today means dx12 + DXR on
# Windows (vulkan RT is stubbed upstream). Where that is missing, a routine simply acquires no shader and
# draws nothing — the same graceful path a broken shader edit takes.
add_library(shaped-viewer
    src/shaped-viewer/shaped-viewer.cc
    src/shaped-viewer/camera.cc
    src/shaped-viewer/resources/resource_managers.cc
    src/shaped-viewer/rendering/raytrace_routine.cc
    src/shaped-viewer/rendering/pathtrace_routine.cc
    src/shaped-viewer/rendering/view_renderer.cc
    src/shaped-viewer/rendering/shaders.cc
)

target_sources(shaped-viewer
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-viewer/fwd.hh
    src/shaped-viewer/all.hh
    src/shaped-viewer/view_id.hh
    src/shaped-viewer/camera.hh
    src/shaped-viewer/pbr_material.hh
    src/shaped-viewer/light.hh
    src/shaped-viewer/background.hh
    src/shaped-viewer/render_settings.hh
    src/shaped-viewer/scene_item.hh
    src/shaped-viewer/view.hh
    src/shaped-viewer/viewer_definition.hh
    src/shaped-viewer/resources/resource_ids.hh
    src/shaped-viewer/resources/resource_managers.hh
    src/shaped-viewer/resources/impl/lru_pool.hh
    src/shaped-viewer/rendering/frame_constants.hh
    src/shaped-viewer/rendering/raytrace_routine.hh
    src/shaped-viewer/rendering/pathtrace_routine.hh
    src/shaped-viewer/rendering/view_renderer.hh
    src/shaped-viewer/rendering/shaders.hh
)

target_compile_features(shaped-viewer PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-viewer/...>' works for consumers.
target_include_directories(shaped-viewer
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(shaped-viewer PUBLIC shaped-rendering)

target_compile_options(shaped-viewer
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# sv's own shader package: the ray-tracing shaders (raygen / miss / closest-hit).
# The fullscreen blit lives in shaped-rendering (sr::blit_routine, sr_shaders).
# Must be declared here, in the CMakeLists that defines the target — the generated sources are added as
# PRIVATE sources and add_custom_command(OUTPUT) is directory-scoped. Deliberately not gated on a compiler:
# generation emits C++ descriptors and embeds the sources; compilation happens at runtime through slib.
sc_add_shader_package(
    TARGET     shaped-viewer
    NAME       sv_shaders
    NAMESPACE  sv::shaders
    SOURCE_DIR shaders
    LANGUAGE   hlsl
    SHADERS
        raygen.hlsl:raygen:RayGen
        shading.hlsl:miss:Miss
        shading.hlsl:closest_hit:ClosestHit
        pathtrace.hlsl:raygen:PathTraceRayGen
        pt_hit.hlsl:miss:PtMiss
        pt_hit.hlsl:miss:PtShadowMiss
        pt_hit.hlsl:closest_hit:PtClosestHit
)

# Test executable.
# The vocabulary test needs no device and runs everywhere; the GPU tests drive a dx12 WARP/hardware context
# and compile the ray-tracing shaders through DXC, so they need the dx12 backend and a compiler.
if(SC_BUILD_TESTS)
    add_executable(shaped-viewer-test
        tests/main.cc
        tests/smoke-test.cc
        tests/vocabulary-test.cc
        tests/lru-pool-test.cc
    )

    target_link_libraries(shaped-viewer-test
        PRIVATE
        shaped-viewer
        nexus
    )

    # The ray-traced view tests need a real dx12 device (create_dx12_context) and DXC to compile the shaders.
    # Both compile only where those exist; each SKIPs at runtime when the device reports no ray tracing.
    if(WIN32 AND SC_HAS_DXC_COMPILER AND TARGET shaped-graphics-dx12)
        target_sources(shaped-viewer-test PRIVATE
            tests/raytraced-view-test.cc
            tests/pathtraced-view-test.cc
            tests/view-renderer-test.cc
            tests/viewer-window-manual-test.cc
            tests/pathtraced-window-manual-test.cc
        )
        target_link_libraries(shaped-viewer-test PRIVATE shaped-graphics-dx12)

        # Stage the runtime DLLs (DXC's dxcompiler/dxil, the dx12 backend, SDL3 for the window) next to the
        # binary so it starts. $<TARGET_RUNTIME_DLLS> reaches them through the link chain; empty off Windows.
        add_custom_command(TARGET shaped-viewer-test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:shaped-viewer-test> $<TARGET_FILE_DIR:shaped-viewer-test>
            COMMAND_EXPAND_LISTS
        )
    endif()

    # Browser test runner (Emscripten only); a no-op on native toolchains.
    sc_add_nexus_web_runner(shaped-viewer-test)
endif()
