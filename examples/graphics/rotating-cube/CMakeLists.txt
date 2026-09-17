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

# A standalone page for the web build: one .html file with the wasm inside it, opened straight from disk.
#
# The example target itself is a nexus binary the test presets link for node, which a browser cannot load --
# NODERAWFS alone sees to that -- so the page is a second link of the same sources rather than a wrapper around it.
# SINGLE_FILE embeds the wasm as a data URI, which is what lets `file://` work with no server and no CORS.
if(EMSCRIPTEN AND _cube_backend STREQUAL "shaped-graphics-webgpu" AND TARGET graphics-rotating-cube-example)
    set(_cube_web graphics-rotating-cube-example-web)
    get_target_property(_cube_srcs graphics-rotating-cube-example SOURCES)
    get_target_property(_cube_libs graphics-rotating-cube-example LINK_LIBRARIES)
    get_target_property(_cube_incs graphics-rotating-cube-example INCLUDE_DIRECTORIES)
    get_target_property(_cube_defs graphics-rotating-cube-example COMPILE_DEFINITIONS)

    add_executable(${_cube_web} ${_cube_srcs})
    target_link_libraries(${_cube_web} PRIVATE ${_cube_libs})
    target_include_directories(${_cube_web} PRIVATE ${_cube_incs})
    target_compile_definitions(${_cube_web} PRIVATE ${_cube_defs})

    # nexus runs whatever its command line names, and a page has no command line -- so the page supplies one.
    target_link_options(${_cube_web} PRIVATE
        "SHELL:-s SINGLE_FILE=1"
        "SHELL:-s NODERAWFS=0"
        "SHELL:-s ALLOW_MEMORY_GROWTH=1"
        "SHELL:--pre-js ${CMAKE_CURRENT_SOURCE_DIR}/web-arguments.js"
        "SHELL:--shell-file ${CMAKE_CURRENT_SOURCE_DIR}/web-shell.html"
    )
    set_property(TARGET ${_cube_web} APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/web-arguments.js" "${CMAKE_CURRENT_SOURCE_DIR}/web-shell.html")
    set_target_properties(${_cube_web} PROPERTIES
        SUFFIX ".html"
        OUTPUT_NAME "rotating-cube"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web"
    )
endif()
