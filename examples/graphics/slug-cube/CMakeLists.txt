# Slug shapes on a spinning cube, and a caption over it.
# The labels are sr::slug_routine's quads, and the star is a mesh shader calling module slug's coverage directly.
#
# Gated like sgl-cube: it needs a backend to create a context on and a compiler for that backend's format.
# No webgpu arm, because its text comes from a font file the operating system has, and a page has no filesystem.

sc_resolve_example_backend(_slug_cube_backend _slug_cube_mode SUPPORTS dx12 vulkan metal)

if(_slug_cube_mode STREQUAL "none")
    return()
endif()
if(_slug_cube_mode STREQUAL "stub")
    sc_add_example_backend_stub(graphics-slug-cube-example "${SC_EXAMPLE_BACKEND}" SUPPORTS dx12 vulkan metal)
    return()
endif()

if(_slug_cube_backend STREQUAL "shaped-graphics-metal")
    if(NOT SC_HAS_METAL_COMPILER)
        return()
    endif()
elseif(NOT SC_HAS_DXC_COMPILER)
    return()
endif()

if(_slug_cube_backend STREQUAL "shaped-graphics-dx12")
    set(_slug_cube_backend_is_dx12 1)
else()
    set(_slug_cube_backend_is_dx12 0)
endif()
if(_slug_cube_backend STREQUAL "shaped-graphics-metal")
    set(_slug_cube_backend_is_metal 1)
else()
    set(_slug_cube_backend_is_metal 0)
endif()

sc_add_example(graphics-slug-cube-example
    SOURCES
    slug_cube.cc
    LINK
    shaped-rendering
    ${_slug_cube_backend}
)

target_compile_definitions(graphics-slug-cube-example PRIVATE
    SLUG_CUBE_BACKEND_DX12=${_slug_cube_backend_is_dx12}
    SLUG_CUBE_BACKEND_METAL=${_slug_cube_backend_is_metal}
)

sc_add_shader_package(
    TARGET     graphics-slug-cube-example
    NAME       slug_cube_shaders
    NAMESPACE  shaders
    SOURCE_DIR shaders
    LANGUAGE   sgl
    MODULE_DIRS ${SR_SGL_MODULE_DIR} # sr exports module `slug`; the cube's shader `use`s it
    SHADERS
        slug_cube.sgl:*
)
