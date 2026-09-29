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

# A standalone page for the web build, exactly as rotating-cube makes one: a second link of the same sources.
if(EMSCRIPTEN AND _sgl_cube_backend STREQUAL "shaped-graphics-webgpu" AND TARGET graphics-sgl-cube-example)
    set(_sgl_cube_web graphics-sgl-cube-example-web)
    get_target_property(_sgl_cube_srcs graphics-sgl-cube-example SOURCES)
    get_target_property(_sgl_cube_libs graphics-sgl-cube-example LINK_LIBRARIES)
    get_target_property(_sgl_cube_incs graphics-sgl-cube-example INCLUDE_DIRECTORIES)
    get_target_property(_sgl_cube_defs graphics-sgl-cube-example COMPILE_DEFINITIONS)

    add_executable(${_sgl_cube_web} ${_sgl_cube_srcs})
    target_link_libraries(${_sgl_cube_web} PRIVATE ${_sgl_cube_libs})
    target_include_directories(${_sgl_cube_web} PRIVATE ${_sgl_cube_incs})
    target_compile_definitions(${_sgl_cube_web} PRIVATE ${_sgl_cube_defs})

    target_link_options(${_sgl_cube_web} PRIVATE
        "SHELL:-s SINGLE_FILE=1"
        "SHELL:-s NODERAWFS=0"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/web-arguments.js"
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web-shell.html"
    )
    set_property(TARGET ${_sgl_cube_web} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/web-arguments.js" "${CMAKE_CURRENT_SOURCE_DIR}/web-shell.html")
    set_target_properties(${_sgl_cube_web} PROPERTIES
        SUFFIX ".html"
        OUTPUT_NAME "sgl-cube"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
    )
endif()
