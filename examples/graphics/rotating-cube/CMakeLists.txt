# The smallest program that draws something: sg for the device and the raster path, slib for the shader package,
# sr for the window and for writing the capture image.
#
# Gated rather than broken: it needs a backend to create a context on and a compiler to turn its shaders into what
# that backend accepts, and a cross-compile CI leg builds every target.

# SC_EXAMPLE_BACKEND is declared in the root CMakeLists, with the other SC_* options.
# This example is the one that supports every backend, which is what it exists to exercise -- dx12 first under `auto`.
sc_resolve_example_backend(_cube_backend _cube_mode SUPPORTS dx12 vulkan webgpu)
if(_cube_mode STREQUAL "none")
    return()
endif()
if(_cube_mode STREQUAL "stub")
    sc_add_example_backend_stub(graphics-rotating-cube-example "${SC_EXAMPLE_BACKEND}" SUPPORTS dx12 vulkan webgpu)
    return()
endif()

# The shader language follows the backend, because that is what decides which one the device accepts:
# HLSL through DXC for dx12 and vulkan, WGSL for webgpu -- and WGSL needs no compiler at all, which is why only the
# first pair is gated on DXC.
if(_cube_backend STREQUAL "shaped-graphics-webgpu")
    set(_cube_language wgsl)
elseif(NOT SC_HAS_DXC_COMPILER)
    return()
else()
    set(_cube_language hlsl)
endif()

if(_cube_backend STREQUAL "shaped-graphics-dx12")
    set(_cube_backend_is_dx12 1)
else()
    set(_cube_backend_is_dx12 0)
endif()
if(_cube_backend STREQUAL "shaped-graphics-webgpu")
    set(_cube_backend_is_webgpu 1)
else()
    set(_cube_backend_is_webgpu 0)
endif()

sc_add_example(graphics-rotating-cube-example
    SOURCES
    rotating_cube.cc
    LINK
    shaped-rendering
    ${_cube_backend}
)

# Which backend header the one context-creation call reaches for.
# A define rather than a platform `#if`, so the source says "the backend this build has" and the build system stays
# the only place that decides which one that is.
target_compile_definitions(graphics-rotating-cube-example PRIVATE
    ROTATING_CUBE_BACKEND_DX12=${_cube_backend_is_dx12}
    ROTATING_CUBE_BACKEND_WEBGPU=${_cube_backend_is_webgpu}
)

# The package emits C++ descriptors and embeds the sources; compilation happens at runtime through slib.
#
# The two languages differ in more than their file names: WGSL takes one entry point per file, so the two stages are
# two files, and its `vertex_input` / `constants` mirrors do not exist -- the example writes that layout out instead.
if(_cube_language STREQUAL "wgsl")
    sc_add_shader_package(
        TARGET     graphics-rotating-cube-example
        NAME       cube_shaders
        NAMESPACE  shaders
        SOURCE_DIR shaders
        LANGUAGE   wgsl
        SHADERS
            cube_vs.wgsl:vertex:main_vs
            cube_fs.wgsl:fragment:main_fs
    )
else()
    sc_add_shader_package(
        TARGET     graphics-rotating-cube-example
        NAME       cube_shaders
        NAMESPACE  shaders
        SOURCE_DIR shaders
        LANGUAGE   hlsl
        SHADERS
            cube.hlsl:vertex:main_vs
            cube.hlsl:fragment:main_ps
            cube.hlsl:vertex_input:vs_input
            cube.hlsl:constants:gConstants
    )
endif()

# A standalone page for the web build: a second link of the same sources.
if(_cube_backend STREQUAL "shaped-graphics-webgpu")
    sc_add_example_web_page(graphics-rotating-cube-example
        EXAMPLE shaped-graphics/rotating-cube
        OUTPUT rotating-cube
        SHELL ${CMAKE_CURRENT_SOURCE_DIR}/web-shell.html
    )
endif()
