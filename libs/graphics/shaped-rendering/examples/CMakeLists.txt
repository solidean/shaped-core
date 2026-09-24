# shaped-rendering examples: runnable demonstrations of sr's routines in practice.
# Run one with `uv run dev.py example shaped-rendering/denoise-playground`; see docs/guides/examples.md.
#
# Gated rather than broken: the denoise playground traces through a DXC-compiled compute shader and presents through
# a swapchain, so it needs a backend to create a context on and a compiler to feed it.
# Every CI leg builds all targets, wasm and cross-compile included, so an ungated graphical example would break
# builds that could never have run it.
if(NOT SC_HAS_DXC_COMPILER)
    return()
endif()

# dx12 and vulkan both take HLSL through DXC and both present through a swapchain, so one source serves them.
sc_resolve_example_backend(_sr_backend _sr_mode SUPPORTS dx12 vulkan)
if(_sr_mode STREQUAL "none")
    return()
endif()
if(_sr_mode STREQUAL "stub")
    sc_add_example_backend_stub(shaped-rendering-denoise-playground-example "${SC_EXAMPLE_BACKEND}" SUPPORTS dx12 vulkan)
    return()
endif()

if(_sr_backend STREQUAL "shaped-graphics-dx12")
    set(_sr_backend_is_dx12 1)
else()
    set(_sr_backend_is_dx12 0)
endif()

sc_add_example(shaped-rendering-denoise-playground-example
    SOURCES
    denoise-playground/denoise_playground.cc
    LINK
    shaped-rendering
    ${_sr_backend}
    PCH SG NEXUS
)

# Which backend header the one context-creation call reaches for.
# A define rather than a platform `#if`, so the source says "the backend this build has" and the build system stays
# the only place that decides which one that is.
target_compile_definitions(shaped-rendering-denoise-playground-example PRIVATE
    DENOISE_PLAYGROUND_BACKEND_DX12=${_sr_backend_is_dx12}
)

sc_add_shader_package(
    TARGET     shaped-rendering-denoise-playground-example
    NAME       scene_shaders
    NAMESPACE  shaders
    SOURCE_DIR denoise-playground/shaders
    LANGUAGE   hlsl
    SHADERS
        noisy_scene.hlsl:compute:main_cs
        noisy_scene.hlsl:binding:scene_bindings
        noisy_scene.hlsl:constants:gConstants
        compose.hlsl:compute:main_cs
        compose.hlsl:binding:compose_bindings
        compose.hlsl:constants:gConstants
)
