# How fast inline traces run: hardware ray queries natively, the ray-query polyfill on webgpu.
#
# Gated like sgl-cube: a backend to create a context on, and a compiler for that backend's format.
# metal is left out until someone runs it there.

sc_resolve_example_backend(_ray_throughput_backend _ray_throughput_mode SUPPORTS dx12 vulkan webgpu)

if(_ray_throughput_mode STREQUAL "none")
    return()
endif()
if(_ray_throughput_mode STREQUAL "stub")
    sc_add_example_backend_stub(graphics-ray-throughput-example "${SC_EXAMPLE_BACKEND}" SUPPORTS dx12 vulkan webgpu)
    return()
endif()

# WGSL is the source the webgpu backend compiles itself; dx12 and vulkan go through DXC.
if(NOT _ray_throughput_backend STREQUAL "shaped-graphics-webgpu" AND NOT SC_HAS_DXC_COMPILER)
    return()
endif()

if(_ray_throughput_backend STREQUAL "shaped-graphics-dx12")
    set(_ray_throughput_backend_is_dx12 1)
else()
    set(_ray_throughput_backend_is_dx12 0)
endif()
if(_ray_throughput_backend STREQUAL "shaped-graphics-webgpu")
    set(_ray_throughput_backend_is_webgpu 1)
else()
    set(_ray_throughput_backend_is_webgpu 0)
endif()

sc_add_example(graphics-ray-throughput-example
    SOURCES
    ray_throughput.cc
    LINK
    shaped-rendering
    ${_ray_throughput_backend}
)

target_compile_definitions(graphics-ray-throughput-example PRIVATE
    RAY_THROUGHPUT_BACKEND_DX12=${_ray_throughput_backend_is_dx12}
    RAY_THROUGHPUT_BACKEND_WEBGPU=${_ray_throughput_backend_is_webgpu}
    RAY_THROUGHPUT_BACKEND_METAL=0
)

sc_add_shader_package(
    TARGET     graphics-ray-throughput-example
    NAME       ray_throughput_shaders
    NAMESPACE  shaders
    SOURCE_DIR shaders
    LANGUAGE   sgl
    SHADERS
        ray_throughput.sgl:*
)

# A standalone page for the web build: a second link of the same sources.
if(_ray_throughput_backend STREQUAL "shaped-graphics-webgpu")
    sc_add_example_web_page(graphics-ray-throughput-example
        EXAMPLE shaped-graphics/ray-throughput
        OUTPUT ray-throughput
        SHELL ${CMAKE_CURRENT_SOURCE_DIR}/web-shell.html
    )
endif()
