cmake_minimum_required(VERSION 3.28)
project(shaped-rendering LANGUAGES CXX)

# shaped-rendering (sr::) builds concrete render routines and helpers on top of shaped-graphics (mipmap generation, tonemapping, texture compression, ...).
# The render-routine *framework* (the routine base and the per-context ctx.routines registry) lives in shaped-graphics;
# sr hosts the concrete routines, which land in their own .cc files as they are implemented.
# It is also home to the window abstraction.
add_library(shaped-rendering
    src/shaped-rendering/shaped-rendering.cc
    src/shaped-rendering/capture.cc
    src/shaped-rendering/window.cc
    src/shaped-rendering/blit_routine.cc
    src/shaped-rendering/box_filter_mipmap_routine.cc
    src/shaped-rendering/reconstruct.cc
    src/shaped-rendering/atrous_denoise_routine.cc
    src/shaped-rendering/svgf_denoise_routine.cc
    src/shaped-rendering/mix_routine.cc
    src/shaped-rendering/dlss_rr_routine.cc
    src/shaped-rendering/nrd_denoise_routine.cc
    src/shaped-rendering/oidn_denoise_routine.cc
    src/shaped-rendering/fsr_upscale_routine.cc
    src/shaped-rendering/impl/tza.cc
    src/shaped-rendering/impl/oidn_network.cc
    src/shaped-rendering/raster_box_filter_mipmap_routine.cc
    src/shaped-rendering/imgui_context.cc
    src/shaped-rendering/imgui_routine.cc
    src/shaped-rendering/imgui_style.cc
    src/shaped-rendering/shaders.cc
    src/shaped-rendering/slug_atlas.cc
    src/shaped-rendering/slug_font.cc
    src/shaped-rendering/slug_routine.cc
    src/shaped-rendering/slug_shape.cc
    src/shaped-rendering/impl/slug_reference.cc
    src/shaped-rendering/impl/imgui_draw_math.cc
    src/shaped-rendering/impl/imgui_input_translation.cc
    src/shaped-rendering/impl/imgui_texture_registry.cc
)

target_sources(shaped-rendering
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-rendering/fwd.hh
    src/shaped-rendering/all.hh
    src/shaped-rendering/input.hh
    src/shaped-rendering/blit_routine.hh
    src/shaped-rendering/box_filter_mipmap_routine.hh
    src/shaped-rendering/reconstruct.hh
    src/shaped-rendering/atrous_denoise_routine.hh
    src/shaped-rendering/svgf_denoise_routine.hh
    src/shaped-rendering/mix_routine.hh
    src/shaped-rendering/dlss_rr_routine.hh
    src/shaped-rendering/nrd_denoise_routine.hh
    src/shaped-rendering/oidn_denoise_routine.hh
    src/shaped-rendering/fsr_upscale_routine.hh
    src/shaped-rendering/impl/fsr_backend.hh
    src/shaped-rendering/impl/denoise_images.hh
    src/shaped-rendering/raster_box_filter_mipmap_routine.hh
    src/shaped-rendering/window.hh
    src/shaped-rendering/imgui_context.hh
    src/shaped-rendering/imgui_routine.hh
    src/shaped-rendering/imgui_style.hh
    src/shaped-rendering/shaders.hh
    src/shaped-rendering/slug_atlas.hh
    src/shaped-rendering/slug_font.hh
    src/shaped-rendering/slug_routine.hh
    src/shaped-rendering/slug_shape.hh
    src/shaped-rendering/impl/slug_reference.hh
)

target_compile_features(shaped-rendering PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-rendering/...>' works for consumers.
target_include_directories(shaped-rendering
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# sr depends on slib because concrete routines acquire their shaders through it (a routine-author dependency).
# The routine framework in sg needs no slib — reload tracking is sg's own generation counter.
# slib is added after us in the root CMakeLists, which is fine — target_link_libraries resolves item names at generate time (shaped-graphics-test does the same).
#
# imgui is PUBLIC on three counts, all pointing the same way:
# an application writes ImGui::Begin(...) in its own TUs,
# sr's public headers name ImDrawData,
# and imgui's target carries PUBLIC compile definitions that change what imgui.h declares — a consumer that missed them would see a different ImGuiIO layout than imgui.cpp was built with.
target_link_libraries(shaped-rendering PUBLIC shaped-graphics shaped-shader-library imgui)

# babel owns image encoding, which capture writes with, and font reading, which Slug's glyph compilation reads from.
# PUBLIC because slug_shape.hh and slug_font.hh take a babel::font::face.
target_link_libraries(shaped-rendering PUBLIC babel-serializer)

sc_target_pch(shaped-rendering SG)

target_compile_options(shaped-rendering
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# The window API is always present.
# Only its backend is optional, because SDL3 is fetched on demand.
# Without one, window_system::try_create fails instead of the type disappearing — a caller writes the same code either way and finds out at run time, rather than gating on a macro.
if(SC_HAS_SDL3)
    target_sources(shaped-rendering
        PRIVATE
        src/shaped-rendering/impl/input_translation.cc
        src/shaped-rendering/impl/window_sdl.cc
    )

    # SDL links PRIVATE: an implementation detail of impl/, reaching no public header.
    target_link_libraries(shaped-rendering PRIVATE SDL3::SDL3)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_WINDOW=1)
else()
    target_sources(shaped-rendering PRIVATE src/shaped-rendering/impl/window_null.cc)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_WINDOW=0)
endif()

# NRD, when somebody has fetched its sources (extern/nrd/fetch-nrd.py).
#
# It links PRIVATE and reaches impl/ alone, like every other vendor SDK here.
# No backend gate: NRD hands back a LIST of dispatches and shaped-rendering executes them through sg, so unlike DLSS
# this member needs no native scope and runs on any dx12 adapter, WARP included.
if(TARGET NRD)
    target_sources(shaped-rendering PRIVATE src/shaped-rendering/impl/nrd_instance.cc
                                        src/shaped-rendering/impl/nrd_session.cc)
    target_link_libraries(shaped-rendering PRIVATE NRD)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_NRD=1)
else()
    target_sources(shaped-rendering PRIVATE src/shaped-rendering/impl/nrd_null.cc)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_NRD=0)
endif()
# The trained OIDN weights the member runs, as a path rather than a target: nothing links them, and the member reads
# the blob at run time.
# Empty when they were not fetched, which makes the member report `unsupported` rather than fail to build.
set(SR_OIDN_WEIGHTS_DIR_VALUE "")
if(OIDN_WEIGHTS_DIR)
    set(SR_OIDN_WEIGHTS_DIR_VALUE "${OIDN_WEIGHTS_DIR}")
endif()
target_compile_definitions(shaped-rendering PRIVATE SR_OIDN_WEIGHTS_DIR="${SR_OIDN_WEIGHTS_DIR_VALUE}")
# NVIDIA DLSS Ray Reconstruction, when somebody has fetched the SDK (extern/dlss/fetch-dlss.py).
#
# It needs a dx12 native scope to hand NGX a command list, so it is built only where that backend is, and it links
# PRIVATE: nvsdk_ngx.h reaches impl/ and no public header of ours.
# Without it the member's routine still EXISTS — every symbol a caller names is always there — and reports
# `unsupported`, which is the same shape `sr::window_system` takes without SDL3.
if(TARGET dlss AND TARGET shaped-graphics-dx12)
    target_sources(shaped-rendering PRIVATE src/shaped-rendering/impl/dlss_ngx.cc)
    target_link_libraries(shaped-rendering PRIVATE dlss shaped-graphics-dx12)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_DLSS=1)

    # The runtime has to sit next to whatever loads it, and it is an IMPORTED target so $<TARGET_RUNTIME_DLLS> cannot
    # see it — the path travels on the target instead.
    #
    # Cached rather than local, so a consumer's own CMakeLists can stage it beside its binary: NGX searches the
    # executable's directory, and a test or an example is a different directory from this library's.
    get_target_property(_dlss_runtime dlss SC_DLSS_RUNTIME)
    set(SC_DLSS_RUNTIME "${_dlss_runtime}" CACHE INTERNAL "nvngx_dlssd.dll, to stage beside anything that loads NGX")
else()
    target_sources(shaped-rendering PRIVATE src/shaped-rendering/impl/dlss_null.cc)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_DLSS=0)

    # Cleared rather than left alone: the cache outlives a configure, so a build directory that once had the SDK would
    # otherwise keep staging a DLL nothing loads — and fail the copy outright once the file is gone.
    unset(SC_DLSS_RUNTIME CACHE)
endif()

# AMD FSR 3.1's upscaler, where its sources were fetched (extern/fidelityfx).
#
# AMD's host code joins this library's objects and reaches the GPU only through the backend in impl/fsr_backend.cc.
# Its passes are compiled like any other shader here, one wrapper per pass: the permutation defines, then AMD's pass.
# The permutation is fixed because the context flags sr creates FSR with are — HDR input, low-resolution motion,
# inverted depth — so exactly one variant of each pass is ever asked for, and fsr_backend.cc refuses any other.
# The wrappers go beside the flattened sources extern/fidelityfx copied in, which is gitignored for the same reason.
set(_sr_fsr_shaders)
if(TARGET fidelityfx-fsr3)
    set(_sr_fsr_dir "${CMAKE_CURRENT_SOURCE_DIR}/shaders/fsr3")
    foreach(_pass IN ITEMS prepare_inputs luma_pyramid shading_change_pyramid shading_change prepare_reactivity
                           luma_instability accumulate accumulate_sharpen rcas debug_view autogen_reactive)
        set(_source "${_pass}")
        set(_sharpen 0)
        if(_pass STREQUAL "accumulate_sharpen")
            set(_source "accumulate")
            set(_sharpen 1)
        endif()
        set(_text "// Generated by libs/graphics/shaped-rendering/CMakeLists.txt: FSR 3.1's ${_pass} pass, in the one\n")
        string(APPEND _text "// permutation sr builds. The defines are those of AMD's BuildFSR3UpscalerShaders.bat for fp32 and wave32.\n")
        foreach(_define IN ITEMS
                "FFX_GPU 1" "FFX_HLSL 1" "FFX_HALF 0" "FFX_HLSL_SM 62" "FFX_SPD_NO_WAVE_OPERATIONS 1"
                "FFX_IMPLICIT_SHADER_REGISTER_BINDING_HLSL 0" "FFX_FSR3UPSCALER_EMBED_ROOTSIG 0"
                "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF 0"
                "FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF 0"
                "FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF 1"
                "FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF 0"
                "FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 2"
                "FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 0"
                "FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 1"
                "FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1"
                "FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0"
                "FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 1"
                "FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING ${_sharpen}")
            string(APPEND _text "#define ${_define}\n")
        endforeach()
        string(APPEND _text "#include \"ffx_fsr3upscaler_${_source}_pass.hlsl\"\n")
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fsr3-staging/${_pass}.hlsl" "${_text}")
        file(COPY_FILE "${CMAKE_CURRENT_BINARY_DIR}/fsr3-staging/${_pass}.hlsl" "${_sr_fsr_dir}/${_pass}.hlsl"
             ONLY_IF_DIFFERENT)
        list(APPEND _sr_fsr_shaders "fsr3/${_pass}.hlsl:compute:CS")
    endforeach()

    target_sources(shaped-rendering PRIVATE src/shaped-rendering/impl/fsr_backend.cc)
    target_link_libraries(shaped-rendering PRIVATE fidelityfx-fsr3)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_FSR=1)
else()
    target_sources(shaped-rendering PRIVATE src/shaped-rendering/impl/fsr_null.cc)
    target_compile_definitions(shaped-rendering PUBLIC SR_HAS_FSR=0)
endif()

# sr's own shader packages, one per language: a package has one.
# Must be declared here, in the CMakeLists that defines the target: the generated sources are added as PRIVATE sources and add_custom_command(OUTPUT) is directory-scoped.
# A routine's shaders go in the SGL package, which reaches dx12, vulkan, webgpu and metal from one source.
# The HLSL package holds the shaders not yet ported; docs/TODO.md says what holds each back.
#
# Deliberately not gated on a compiler existing.
# Package generation needs none — it emits C++ descriptors and embeds the sources;
# compilation happens at runtime through slib.
# Without a compiler the routine simply acquires no shader and draws nothing, which is the same graceful path a broken shader edit takes.
#
# The NRD repack pass is the one conditional entry.
# It includes NRD's own packing header, which only exists once somebody has fetched the SDK, so listing it
# unconditionally would turn a missing optional dependency into a configure error.
set(_sr_nrd_shaders)
if(TARGET NRD)
    set(_sr_nrd_shaders
        nrd_repack.hlsl:compute:main_cs
        nrd_repack.hlsl:binding:nrd_repack_bindings
        nrd_repack.hlsl:constants:gConstants
        nrd_resolve.hlsl:compute:main_cs
        nrd_resolve.hlsl:binding:nrd_resolve_bindings)
endif()

sc_add_shader_package(
    TARGET     shaped-rendering
    NAME       sr_sgl_shaders
    NAMESPACE  sr::sgl_shaders
    SOURCE_DIR shaders
    LANGUAGE   sgl
    MODULE_DIRS shaders/modules
    SHADERS
        module:slug
        blit.sgl:*
        imgui.sgl:*
        raster_box_filter_mipmap.sgl:*
        nn_pool.sgl:*
        nn_upsample.sgl:*
        slug_quads.sgl:*
)

# sr's SGL modules, which another package `use`s by naming this directory in its own MODULE_DIRS.
# Module `slug` is Slug's coverage: a mesh's own pixel shader draws a shape on its surface through it.
set(SR_SGL_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/modules" CACHE INTERNAL "sr's SGL module directory")

sc_add_shader_package(
    TARGET     shaped-rendering
    NAME       sr_shaders
    NAMESPACE  sr::shaders
    SOURCE_DIR shaders
    LANGUAGE   hlsl
    SHADERS
        box_filter_mipmap.hlsl:compute:main_1d_cs
        box_filter_mipmap.hlsl:compute:main_1d_array_cs
        box_filter_mipmap.hlsl:compute:main_2d_cs
        box_filter_mipmap.hlsl:compute:main_2d_array_cs
        box_filter_mipmap.hlsl:compute:main_3d_cs
        atrous_denoise.hlsl:compute:main_cs
        atrous_denoise.hlsl:binding:atrous_bindings
        atrous_denoise.hlsl:constants:gConstants
        svgf_temporal.hlsl:compute:main_cs
        svgf_temporal.hlsl:binding:svgf_temporal_bindings
        svgf_temporal.hlsl:constants:gConstants
        svgf_variance.hlsl:compute:main_cs
        svgf_variance.hlsl:binding:svgf_variance_bindings
        svgf_variance.hlsl:constants:gConstants
        svgf_atrous.hlsl:compute:main_cs
        svgf_atrous.hlsl:binding:svgf_atrous_bindings
        svgf_atrous.hlsl:constants:gConstants
        mix.hlsl:compute:main_cs
        mix.hlsl:binding:mix_bindings
        mix.hlsl:constants:gConstants
        fsr_prepare_depth.hlsl:compute:main_cs
        fsr_prepare_depth.hlsl:binding:fsr_prepare_depth_bindings
        fsr_prepare_depth.hlsl:constants:gConstants
        fsr_clear_float.hlsl:compute:main_cs
        fsr_clear_float.hlsl:binding:fsr_clear_float_bindings
        fsr_clear_float.hlsl:constants:gConstants
        fsr_clear_uint.hlsl:compute:main_cs
        fsr_clear_uint.hlsl:binding:fsr_clear_uint_bindings
        fsr_clear_uint.hlsl:constants:gConstants
        ${_sr_fsr_shaders}
        ${_sr_nrd_shaders}
        nn_conv.hlsl:compute:main_cs
        nn_conv.hlsl:binding:nn_conv_bindings
        nn_conv.hlsl:constants:gConstants
        nn_input.hlsl:compute:main_cs
        nn_input.hlsl:binding:nn_input_bindings
        nn_input.hlsl:constants:gConstants
        nn_output.hlsl:compute:main_cs
        nn_output.hlsl:binding:nn_output_bindings
        nn_output.hlsl:constants:gConstants
)

# Test executable.
# The window/input tests and the imgui arithmetic (scissor clamping, ortho projection, texture-rect packing) need no device and run everywhere;
# the end-to-end imgui test needs a real one.
if(SC_BUILD_TESTS)
    # These compile in either build: they use only the public API, and guard on SR_HAS_WINDOW the few cases that need a live backend.
    add_executable(shaped-rendering-test
        tests/main.cc
        tests/smoke-test.cc
        tests/input-test.cc
        tests/window-test.cc
        tests/window-manual-test.cc
        tests/imgui-registry-test.cc
        tests/imgui-input-test.cc
        tests/imgui-settings-test.cc
        tests/imgui-style-test.cc
        tests/shader-package-test.cc
        tests/slug-font-test.cc
        tests/slug-module-test.cc
        tests/slug-shape-test.cc
        tests/tza-test.cc
        tests/oidn-tiling-test.cc
    )
    sc_nexus_binary(shaped-rendering-test KINDS tests ${SG_WARP_TEST_BINARY_ARGS})

    target_link_libraries(shaped-rendering-test
        PRIVATE
        shaped-rendering
        nexus
        shaped-graphics-language # the slug module's own tests run on SGL's interpreter
    )
    target_compile_definitions(shaped-rendering-test PRIVATE SR_SGL_MODULE_DIR="${SR_SGL_MODULE_DIR}")
    target_include_directories(shaped-rendering-test PRIVATE "${BABEL_TEST_DIR}") # font/font_builder.hh, for crafted fonts

    sc_target_pch(shaped-rendering-test SG NEXUS) # 0.44

    if(SC_HAS_SDL3)
        # These two reach into impl/ and see SDL headers directly, so they exist only with a backend.
        target_sources(shaped-rendering-test
            PRIVATE
            tests/input-dispatch-test.cc
            tests/input-translation-test.cc
        )

        # The sanctioned place for SDL besides the backend itself — see docs/coding-guidelines.md.
        # sr links SDL PRIVATE, so the test needs its own link to get the include directories.
        target_link_libraries(shaped-rendering-test PRIVATE SDL3::SDL3)

        # Stage SDL3.dll next to the test binary so it starts.
        # SDL3::SDL3 is a real (not imported) shared target, so $<TARGET_RUNTIME_DLLS> reaches it through sr's PRIVATE link — no hand-listed DLL variable, unlike the DXC wiring.
        # The expression is empty off Windows.
        sc_stage_runtime_dlls(shaped-rendering-test DLLS "$<TARGET_RUNTIME_DLLS:shaped-rendering-test>")
    endif()

    # The GPU tests run once per backend this build has: tests/backends/ holds one entry driver per backend, each
    # invoking every sg::context_handle test on one context of it.
    # The tests above need no device and run everywhere.
    set(_sr_test_backends "")
    if(TARGET shaped-graphics-dx12)
        target_sources(shaped-rendering-test PRIVATE tests/backends/dx12-entry.cc)
        list(APPEND _sr_test_backends shaped-graphics-dx12)
    endif()
    if(TARGET shaped-graphics-vulkan)
        target_sources(shaped-rendering-test PRIVATE tests/backends/vulkan-entry.cc)
        list(APPEND _sr_test_backends shaped-graphics-vulkan)
    endif()
    if(TARGET shaped-graphics-metal)
        target_sources(shaped-rendering-test PRIVATE tests/backends/metal-entry.cc)
        list(APPEND _sr_test_backends shaped-graphics-metal)
        # main() arms Metal's validation layer, which is read from the environment before the framework initializes.
        target_compile_definitions(shaped-rendering-test PRIVATE SR_TEST_HAS_METAL)
    endif()
    if(TARGET shaped-graphics-webgpu)
        target_sources(shaped-rendering-test PRIVATE tests/backends/webgpu-entry.cc)
        list(APPEND _sr_test_backends shaped-graphics-webgpu)
    endif()

    # The routines whose shaders are SGL, which every backend compiles with no toolchain beyond slib's own.
    if(_sr_test_backends)
        target_sources(shaped-rendering-test PRIVATE
            tests/backends/backends.cc
            tests/shader_fixtures.cc
            tests/blit-routine-test.cc
            tests/imgui-routine-test.cc
            tests/raster-box-filter-mipmap-test.cc
            tests/nn-resample-test.cc
            tests/slug-routine-test.cc
        )
        target_link_libraries(shaped-rendering-test PRIVATE ${_sr_test_backends})
    endif()

    # A person looks at this one, through a dx12 window.
    if(TARGET shaped-graphics-dx12)
        target_sources(shaped-rendering-test PRIVATE tests/imgui-manual-test.cc)
    endif()

    # The routines whose shaders are still HLSL need DXC, which reaches dx12 and vulkan and nothing else.
    # A build with DXC has no other backend, so these never meet a context their shaders cannot reach.
    if(_sr_test_backends AND SC_HAS_DXC_COMPILER)
        target_sources(shaped-rendering-test PRIVATE
            tests/box-filter-mipmap-test.cc
            tests/denoise-test.cc
            tests/fsr-test.cc
            tests/mix-test.cc
            tests/nrd-test.cc
            tests/oidn-test.cc
            tests/nn-conv-test.cc
            tests/nn-ops-test.cc
            tests/oidn-network-test.cc
        )

        # Intel's own filter, as the oracle the OIDN member is measured against, where it was fetched on request.
        # Linked here and nowhere else: the member runs the weights in its own shaders and never calls the library.
        # Its facade loads its core and its CPU device module by name out of its own directory, so all of them have to
        # sit beside the binary rather than only the one the link names.
        if(TARGET oidn)
            target_sources(shaped-rendering-test PRIVATE tests/oidn_reference.cc)
            target_link_libraries(shaped-rendering-test PRIVATE oidn)
            if(OIDN_RUNTIME_LIBRARIES)
                sc_stage_runtime_dlls(shaped-rendering-test DLLS ${OIDN_RUNTIME_LIBRARIES} TAG oidn)
            endif()
        else()
            target_sources(shaped-rendering-test PRIVATE tests/oidn_reference_null.cc)
        endif()
    endif()

    # slib imports dxcompiler.dll wherever DXC was fetched, so the binary does not start without it, and dxil.dll signs what it emits.
    # Staged here rather than left to the SDL3 block's $<TARGET_RUNTIME_DLLS>, which a build without SDL3 never runs.
    if(WIN32 AND SC_HAS_DXC_COMPILER)
        sc_stage_runtime_dlls(shaped-rendering-test TAG shaped-rendering-test-dxc DLLS ${DXC_RUNTIME_DLLS})
    endif()

    # NGX loads its network runtime from the directory of the binary that initialized it, so the DLL has to be here
    # rather than beside the library.
    if(SC_DLSS_RUNTIME)
        sc_stage_runtime_dlls(shaped-rendering-test DLLS "${SC_DLSS_RUNTIME}" TAG dlss)
    endif()

    target_compile_definitions(shaped-rendering-test PRIVATE SR_OIDN_WEIGHTS_DIR="${SR_OIDN_WEIGHTS_DIR_VALUE}")

    # The one test that crosses sr's window into metal's windowed presentation, which nothing else covers.
    # SDL is what hands out the CAMetalLayer, so it needs a window backend as well as the metal one.
    if(APPLE AND SC_HAS_SDL3 AND TARGET shaped-graphics-metal)
        target_sources(shaped-rendering-test PRIVATE tests/metal-window-manual-test.cc)
    endif()

    # Browser test runner (Emscripten only); a no-op on native toolchains.
    sc_add_nexus_web_runner(shaped-rendering-test)
endif()

add_subdirectory(examples)
