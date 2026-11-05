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

# A standalone page for the web build, as sgl-cube makes one: a second link of the same sources.
if(EMSCRIPTEN AND _ray_throughput_backend STREQUAL "shaped-graphics-webgpu" AND TARGET graphics-ray-throughput-example)
    set(_ray_throughput_web graphics-ray-throughput-example-web)
    get_target_property(_ray_throughput_srcs graphics-ray-throughput-example SOURCES)
    get_target_property(_ray_throughput_libs graphics-ray-throughput-example LINK_LIBRARIES)
    get_target_property(_ray_throughput_incs graphics-ray-throughput-example INCLUDE_DIRECTORIES)
    get_target_property(_ray_throughput_defs graphics-ray-throughput-example COMPILE_DEFINITIONS)

    add_executable(${_ray_throughput_web} ${_ray_throughput_srcs})
    target_link_libraries(${_ray_throughput_web} PRIVATE ${_ray_throughput_libs})
    target_include_directories(${_ray_throughput_web} PRIVATE ${_ray_throughput_incs})
    target_compile_definitions(${_ray_throughput_web} PRIVATE ${_ray_throughput_defs})

    target_link_options(${_ray_throughput_web} PRIVATE
        "SHELL:-s SINGLE_FILE=1"
        "SHELL:-s NODERAWFS=0"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/web-arguments.js"
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web-shell.html"
    )
    set_property(TARGET ${_ray_throughput_web} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/web-arguments.js" "${CMAKE_CURRENT_SOURCE_DIR}/web-shell.html")
    set_target_properties(${_ray_throughput_web} PROPERTIES
        SUFFIX ".html"
        OUTPUT_NAME "ray-throughput"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
    )
endif()
