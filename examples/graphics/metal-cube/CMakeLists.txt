# The metal twin of graphics/rotating-cube, for the backend that example cannot reach.
#
# rotating-cube gets its shaders from slib, which has no metal language, so on a Mac it resolves to no backend at all.
# This one embeds a metallib compiled ahead of time, the way the metal tier-2 fixtures do.
# It is a stopgap: libs/graphics/shaped-graphics/docs/TODO.md carries the shader-package work that would let
# rotating-cube grow a metal arm and retire this.

# SC_EXAMPLE_BACKEND is declared in the root CMakeLists, with the other SC_* options.
sc_resolve_example_backend(_metal_cube_backend _metal_cube_mode SUPPORTS metal)
if(_metal_cube_mode STREQUAL "none")
    return()
endif()
if(_metal_cube_mode STREQUAL "stub")
    sc_add_example_backend_stub(graphics-metal-cube-example "${SC_EXAMPLE_BACKEND}" SUPPORTS metal)
    return()
endif()

sc_add_example(graphics-metal-cube-example
    SOURCES
    metal_cube.cc
    LINK
    shaped-rendering
    ${_metal_cube_backend}
)
