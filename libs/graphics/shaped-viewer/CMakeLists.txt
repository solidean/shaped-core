cmake_minimum_required(VERSION 3.28)
project(shaped-viewer LANGUAGES CXX)

# shaped-viewer (sv::) is the professional, RTX-enabled visualization renderer with a dev-friendly API,
# built on shaped-rendering. This is the first vertical slice: a per-frame viewer_definition of views, each
# path-traced into its own texture by the view_renderer routine and placed into a window by viewer_renderer.
#
# The library API is present on every platform (the vocabulary, managers, routines and renderer all compile
# cross-platform); actual rendering needs a ray-tracing-capable backend, which today means dx12 + DXR on
# Windows (vulkan RT is stubbed upstream). Where that is missing, a routine simply acquires no shader and
# draws nothing — the same graceful path a broken shader edit takes.
add_library(shaped-viewer
    src/shaped-viewer/shaped-viewer.cc
    src/shaped-viewer/context.cc
    src/shaped-viewer/shader_library.cc
    src/shaped-viewer/scene/light.cc
    src/shaped-viewer/scene/background.cc
    src/shaped-viewer/material/material_type.cc
    src/shaped-viewer/material/material.cc
    src/shaped-viewer/material/material_library.cc
    src/shaped-viewer/material/builtin_material_types.cc
    src/shaped-viewer/material/resolve.cc
    src/shaped-viewer/material/shader_generator.cc
    src/shaped-viewer/view/camera.cc
    src/shaped-viewer/view/camera_controller.cc
    src/shaped-viewer/view/view_data.cc
    src/shaped-viewer/view/view_store.cc
    src/shaped-viewer/layout/box_style.cc
    src/shaped-viewer/layout/solvers.cc
    src/shaped-viewer/layout/layout_tree.cc
    src/shaped-viewer/frame.cc
    src/shaped-viewer/refs.cc
    src/shaped-viewer/interactive.cc
    src/shaped-viewer/capture.cc
    src/shaped-viewer/viewer.cc
    src/shaped-viewer/resources/resource_managers.cc
    src/shaped-viewer/resources/bindless_tables.cc
    src/shaped-viewer/resources/gpu_resource_manager.cc
    src/shaped-viewer/rendering/raytrace_routine.cc
    src/shaped-viewer/rendering/pathtrace_routine.cc
    src/shaped-viewer/rendering/render_plan.cc
    src/shaped-viewer/rendering/layout_routine.cc
    src/shaped-viewer/rendering/view_renderer.cc
    src/shaped-viewer/rendering/viewer_renderer.cc
    src/shaped-viewer/rendering/shaders.cc
    src/shaped-viewer/resources/material_shader_cache.cc
    src/shaped-viewer/asset/uri_resolver.cc
    src/shaped-viewer/asset/asset_data.cc
    src/shaped-viewer/asset/asset.cc
    src/shaped-viewer/asset/asset_loader.cc
    src/shaped-viewer/asset/impl/tangent_frames.cc
    src/shaped-viewer/asset/impl/gltf_import.cc
    src/shaped-viewer/asset/impl/obj_import.cc
    src/shaped-viewer/asset/impl/stl_import.cc
    src/shaped-viewer/asset/impl/asset_materials.cc
)

target_sources(shaped-viewer
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-viewer/fwd.hh
    src/shaped-viewer/all.hh
    src/shaped-viewer/frame.hh
    src/shaped-viewer/refs.hh
    src/shaped-viewer/interactive.hh
    src/shaped-viewer/viewer.hh
    src/shaped-viewer/context.hh
    src/shaped-viewer/shader_library.hh
    src/shaped-viewer/scene/pbr_material.hh
    src/shaped-viewer/scene/light.hh
    src/shaped-viewer/scene/background.hh
    src/shaped-viewer/scene/triangle_geometry.hh
    src/shaped-viewer/scene/resident_mesh.hh
    src/shaped-viewer/scene/mesh.hh
    src/shaped-viewer/asset/uri_resolver.hh
    src/shaped-viewer/asset/asset_data.hh
    src/shaped-viewer/asset/asset.hh
    src/shaped-viewer/asset/asset_loader.hh
    src/shaped-viewer/scene/mesh_attribute.hh
    src/shaped-viewer/scene/mesh_flags.hh
    src/shaped-viewer/scene/mesh_texture.hh
    src/shaped-viewer/material/material_attribute.hh
    src/shaped-viewer/material/material_type.hh
    src/shaped-viewer/material/material.hh
    src/shaped-viewer/material/material_library.hh
    src/shaped-viewer/material/resolve.hh
    src/shaped-viewer/material/shader_generator.hh
    src/shaped-viewer/material/impl/material_hash.hh
    src/shaped-viewer/scene/scene_item.hh
    src/shaped-viewer/view/view_id.hh
    src/shaped-viewer/view/camera.hh
    src/shaped-viewer/view/camera_controller.hh
    src/shaped-viewer/view/render_settings.hh
    src/shaped-viewer/view/post_process.hh
    src/shaped-viewer/view/layer.hh
    src/shaped-viewer/view/view_data.hh
    src/shaped-viewer/view/viewer_definition.hh
    src/shaped-viewer/layout/box_style.hh
    src/shaped-viewer/layout/solvers.hh
    src/shaped-viewer/layout/layout_tree.hh
    src/shaped-viewer/resources/resource_data.hh
    src/shaped-viewer/resources/instance_data.hh
    src/shaped-viewer/impl/content_hash.hh
    src/shaped-viewer/impl/keyed_cache.hh
    src/shaped-viewer/impl/view_state.hh
    src/shaped-viewer/resources/resource_managers.hh
    src/shaped-viewer/resources/bindless_tables.hh
    src/shaped-viewer/resources/gpu_resource_manager.hh
    src/shaped-viewer/resources/impl/lru_pool.hh
    src/shaped-viewer/rendering/frame_constants.hh
    src/shaped-viewer/rendering/raytrace_routine.hh
    src/shaped-viewer/rendering/pathtrace_routine.hh
    src/shaped-viewer/rendering/render_plan.hh
    src/shaped-viewer/rendering/layout_routine.hh
    src/shaped-viewer/rendering/view_renderer.hh
    src/shaped-viewer/rendering/viewer_renderer.hh
    src/shaped-viewer/rendering/shaders.hh
    src/shaped-viewer/resources/material_shader_cache.hh
)

target_compile_features(shaped-viewer PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-viewer/...>' works for consumers.
target_include_directories(shaped-viewer
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# babel-serializer is what reads the asset formats sv imports (glTF, OBJ) and decodes their images.
# PUBLIC because `asset_loader` takes an already-parsed `babel::gltf::data` / `babel::obj::data`, so a caller who reads a
# document for their own reasons can hand it straight over rather than re-reading it.
# A legal direction — data/ sits below graphics/ — and the first time sv links anything below shaped-rendering.
target_link_libraries(shaped-viewer PUBLIC shaped-rendering babel-serializer)

# sv brings its own context up when a caller sets no sv::acquire_context, so it needs one backend to fall back on.
# PRIVATE: the fallback is an implementation detail, and a consumer that wants a different backend overrides the hook
# rather than inheriting this one. Which backend the default reaches for is a per-target choice that will grow.
if(WIN32 AND TARGET shaped-graphics-dx12)
    target_link_libraries(shaped-viewer PRIVATE shaped-graphics-dx12)
    target_compile_definitions(shaped-viewer PRIVATE SV_HAS_DEFAULT_BACKEND=1)
else()
    target_compile_definitions(shaped-viewer PRIVATE SV_HAS_DEFAULT_BACKEND=0)
endif()

sc_target_pch(shaped-viewer SG)

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
        layout.hlsl:vertex:main_vs
        layout.hlsl:fragment:border_ps
        layout.hlsl:fragment:view_ps
        layout.hlsl:fragment:wipe_ps
        layout.hlsl:binding:layout_bindings
        layout.hlsl:constants:constants
        pathtrace.hlsl:raygen:PathTraceRayGen
        pt_hit.hlsl:miss:PtMiss
        pt_hit.hlsl:miss:PtShadowMiss
        common.hlsli:binding:flat_bindings
)

# The examples corpus: the viewer as a caller actually reaches for it.
# The subdirectory gates itself on the window + dx12 + DXC combination an example needs, and
# sc_add_single_file_examples is a no-op when SC_BUILD_EXAMPLES is off.
add_subdirectory(examples)

# Test executable.
# The vocabulary test needs no device and runs everywhere; the GPU tests drive a dx12 WARP/hardware context
# and compile the ray-tracing shaders through DXC, so they need the dx12 backend and a compiler.
if(SC_BUILD_TESTS)
    add_executable(shaped-viewer-test
        tests/main.cc
        tests/smoke-test.cc
        tests/vocabulary-test.cc
        tests/lru-pool-test.cc
        tests/keyed-cache-test.cc
        tests/camera-controller-test.cc
        tests/layout-test.cc
        tests/render-plan-test.cc
        tests/view-convergence-test.cc
        tests/bindless-tables-test.cc
        tests/material-resolution-test.cc
        tests/material-shader-generator-test.cc
        tests/material-shader-compile-test.cc
        tests/material-shader-cache-test.cc
        tests/asset-loading-test.cc
        tests/shader-package-test.cc
    )

    target_link_libraries(shaped-viewer-test
        PRIVATE
        shaped-viewer
        nexus
        babel-serializer # the capture test decodes the image back, which is how it catches a truncated write
    )

    sc_target_pch(shaped-viewer-test SG NEXUS) # 0.53

    # The ray-traced view tests need a real dx12 device (create_dx12_context) and DXC to compile the shaders.
    # Both compile only where those exist; each SKIPs at runtime when the device reports no ray tracing.
    if(WIN32 AND SC_HAS_DXC_COMPILER AND TARGET shaped-graphics-dx12)
        target_sources(shaped-viewer-test PRIVATE
            tests/raytraced-view-test.cc
            tests/headless-viewer-test.cc
            tests/pathtraced-view-test.cc
            tests/openpbr-bsdf-test.cc
            tests/volumetric-furnace-test.cc
            tests/view-renderer-test.cc
            tests/view-accumulation-test.cc
            tests/viewer-renderer-test.cc
            tests/layout-routine-test.cc
            tests/gpu-resource-manager-test.cc
            tests/viewer-window-manual-test.cc
            tests/pathtraced-window-manual-test.cc
            tests/immediate-viewer-window-manual-test.cc
            tests/interactive-showcase-manual-test.cc
        )
        target_link_libraries(shaped-viewer-test PRIVATE shaped-graphics-dx12)

        # The BSDF probe: a compute shader over shaders/openpbr.hlsli, so a test can measure what the closure returns.
        # Its own package rather than an entry in sv_shaders, because nothing a viewer runs dispatches it — and a package
        # is per target, so the two mount side by side over the same source directory.
        sc_add_shader_package(
            TARGET     shaped-viewer-test
            NAME       sv_test_shaders
            NAMESPACE  sv_test::shaders
            SOURCE_DIR shaders
            LANGUAGE   hlsl
            SHADERS
                bsdf_probe.hlsl:compute:BsdfProbe
                bsdf_probe.hlsl:binding:probe_bindings
        )

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
