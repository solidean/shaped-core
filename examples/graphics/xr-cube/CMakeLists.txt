# The SGL cube in a headset over OpenXR, on dx12: Quest Link, or any other runtime on Windows.
# A prototype — feat-xr-link-cube-prototype — that talks to OpenXR directly until shaped-xr exists.
#
# Built only where everything it needs is: Windows, the dx12 backend, DXC, and the fetched OpenXR loader.

if(NOT WIN32 OR NOT TARGET openxr_loader OR NOT TARGET shaped-graphics-dx12 OR NOT SC_HAS_DXC_COMPILER)
    return()
endif()

sc_add_example(graphics-xr-cube-example
    SOURCES
    xr_cube.cc
    LINK
    shaped-rendering
    shaped-graphics-dx12
    openxr_loader
)

# sgl-cube's shader, unchanged: the eye's format comes from the host as the window's does there
sc_add_shader_package(
    TARGET     graphics-xr-cube-example
    NAME       xr_cube_shaders
    NAMESPACE  shaders
    SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../sgl-cube/shaders
    LANGUAGE   sgl
    SHADERS
        cube.sgl:*
)

# the loader is a DLL, found beside the executable
add_custom_command(TARGET graphics-xr-cube-example POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:openxr_loader> $<TARGET_FILE_DIR:graphics-xr-cube-example>)
