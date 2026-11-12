# The rotating cube with its shader in SGL: one cube.sgl for every backend, where rotating-cube has a language per backend.
#
# Gated rather than broken, like its sibling: it needs a backend to create a context on and a compiler for that
# backend's format, and a cross-compile CI leg builds every target.

sc_resolve_example_backend(_sgl_cube_backend _sgl_cube_mode SUPPORTS dx12 vulkan webgpu metal)

if(_sgl_cube_mode STREQUAL "none")
    return()
endif()
if(_sgl_cube_mode STREQUAL "stub")
    sc_add_example_backend_stub(graphics-sgl-cube-example "${SC_EXAMPLE_BACKEND}" SUPPORTS dx12 vulkan webgpu metal)
    return()
endif()

# The package's language does NOT follow the backend, which is the point of this example.
# What follows it is the compiler behind the SGL edge: DXC for dx12 and vulkan, ssc::msl for metal, and none at all
# for webgpu.
if(_sgl_cube_backend STREQUAL "shaped-graphics-webgpu")
    # nothing to require: WGSL is the source the backend compiles itself
elseif(_sgl_cube_backend STREQUAL "shaped-graphics-metal")
    if(NOT SC_HAS_METAL_COMPILER)
        return()
    endif()
elseif(NOT SC_HAS_DXC_COMPILER)
    return()
endif()

if(_sgl_cube_backend STREQUAL "shaped-graphics-dx12")
    set(_sgl_cube_backend_is_dx12 1)
else()
    set(_sgl_cube_backend_is_dx12 0)
endif()
if(_sgl_cube_backend STREQUAL "shaped-graphics-webgpu")
    set(_sgl_cube_backend_is_webgpu 1)
else()
    set(_sgl_cube_backend_is_webgpu 0)
endif()
if(_sgl_cube_backend STREQUAL "shaped-graphics-metal")
    set(_sgl_cube_backend_is_metal 1)
else()
    set(_sgl_cube_backend_is_metal 0)
endif()

sc_add_example(graphics-sgl-cube-example
    SOURCES
    sgl_cube.cc
    LINK
    shaped-rendering
    ${_sgl_cube_backend}
)

# Which backend header the one context-creation call reaches for; the shader side has no such fork.
target_compile_definitions(graphics-sgl-cube-example PRIVATE
    SGL_CUBE_BACKEND_DX12=${_sgl_cube_backend_is_dx12}
    SGL_CUBE_BACKEND_WEBGPU=${_sgl_cube_backend_is_webgpu}
    SGL_CUBE_BACKEND_METAL=${_sgl_cube_backend_is_metal}
)

# One declaration for every backend, and one line for the whole file: its two entry points and its `@inline`
# constants, which the example fills as `shaders::constants`.
sc_add_shader_package(
    TARGET     graphics-sgl-cube-example
    NAME       sgl_cube_shaders
    NAMESPACE  shaders
    SOURCE_DIR shaders
    LANGUAGE   sgl
    SHADERS
        cube.sgl:*
)

# A standalone page for the web build: a second link of the same sources.
if(_sgl_cube_backend STREQUAL "shaped-graphics-webgpu")
    sc_add_example_web_page(graphics-sgl-cube-example
        EXAMPLE shaped-graphics/sgl-cube
        OUTPUT sgl-cube
        SHELL ${CMAKE_CURRENT_SOURCE_DIR}/web-shell.html
        # what web-shell.html hands an edit over with
        LINK_OPTIONS "SHELL:-s EXPORTED_RUNTIME_METHODS=ccall,stringToUTF8,lengthBytesUTF8"
    )
endif()
