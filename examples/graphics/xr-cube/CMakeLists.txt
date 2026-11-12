# The SGL cube in a headset through shaped-xr: vulkan by default, dx12 with `--test-args=--dx12`.
# Built where shaped-xr can create a system, a backend exists, and DXC compiles the shader.

if(NOT TARGET shaped-xr OR NOT TARGET openxr_loader OR NOT SC_HAS_DXC_COMPILER)
    return()
endif()
if(NOT TARGET shaped-graphics-vulkan AND NOT TARGET shaped-graphics-dx12)
    return()
endif()

sc_add_example(graphics-xr-cube-example
    SOURCES
    xr_cube.cc
    LINK
    shaped-xr
)

# both eyes in one draw: view i into layer i, SGL's @view_index
sc_add_shader_package(
    TARGET     graphics-xr-cube-example
    NAME       xr_cube_shaders
    NAMESPACE  shaders
    SOURCE_DIR shaders
    LANGUAGE   sgl
    SHADERS
        xr_cube.sgl:*
)
