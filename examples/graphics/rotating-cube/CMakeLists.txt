# The smallest program that draws something: sg for the device and the raster path, slib for the shader package,
# sr for the window and for writing the capture image.
#
# Gated rather than broken: it needs a backend to create a context on and a compiler to turn cube.hlsl into what
# that backend accepts, and a cross-compile CI leg builds every target.
if(NOT SC_HAS_DXC_COMPILER)
    return()
endif()
if(TARGET shaped-graphics-dx12)
    set(_cube_backend shaped-graphics-dx12)
    set(_cube_backend_is_dx12 1)
elseif(TARGET shaped-graphics-vulkan)
    set(_cube_backend shaped-graphics-vulkan)
    set(_cube_backend_is_dx12 0)
else()
    return()
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
target_compile_definitions(graphics-rotating-cube-example PRIVATE ROTATING_CUBE_BACKEND_DX12=${_cube_backend_is_dx12})

# The package emits C++ descriptors and embeds the sources; compilation happens at runtime through slib.
sc_add_shader_package(
    TARGET     graphics-rotating-cube-example
    NAME       cube_shaders
    NAMESPACE  shaders
    SOURCE_DIR shaders
    LANGUAGE   hlsl
    SHADERS
        cube.hlsl:vertex:main_vs
        cube.hlsl:fragment:main_ps
)
