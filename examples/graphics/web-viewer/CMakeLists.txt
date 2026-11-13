# shaped-viewer in render_mode::raster_bound: the page's canvas on the web, a dx12 context without binding arrays natively.
#
# Gated like ray-throughput: a backend to create a context on, and a compiler for that backend's format.
# The viewer compiles its generated materials at run time, so dx12 needs DXC; webgpu compiles SGL to WGSL itself.

sc_resolve_example_backend(_web_viewer_backend _web_viewer_mode SUPPORTS dx12 webgpu)

if(_web_viewer_mode STREQUAL "none")
    return()
endif()
if(_web_viewer_mode STREQUAL "stub")
    sc_add_example_backend_stub(graphics-web-viewer-example "${SC_EXAMPLE_BACKEND}" SUPPORTS dx12 webgpu)
    return()
endif()

if(NOT _web_viewer_backend STREQUAL "shaped-graphics-webgpu" AND NOT SC_HAS_DXC_COMPILER)
    return()
endif()

if(_web_viewer_backend STREQUAL "shaped-graphics-webgpu")
    set(_web_viewer_backend_is_webgpu 1)
else()
    set(_web_viewer_backend_is_webgpu 0)
endif()

sc_add_example(graphics-web-viewer-example
    SOURCES
    web_viewer.cc
    LINK
    shaped-viewer
    ${_web_viewer_backend}
)

target_compile_definitions(graphics-web-viewer-example PRIVATE WEB_VIEWER_BACKEND_WEBGPU=${_web_viewer_backend_is_webgpu})

# The page's `?model=` arrives over cnet's fetch client.
if(_web_viewer_backend_is_webgpu)
    target_link_libraries(graphics-web-viewer-example PRIVATE clean-net)
endif()

# A standalone page for the web build: a second link of the same sources.
if(_web_viewer_backend STREQUAL "shaped-graphics-webgpu")
    sc_add_example_web_page(graphics-web-viewer-example
        EXAMPLE graphics/web-viewer
        OUTPUT web-viewer
        SHELL ${CMAKE_CURRENT_SOURCE_DIR}/web-shell.html
        LINK_OPTIONS
            # The page embeds sv's SGL sources, whose `<` the HTML minifier reads as a tag.
            "SHELL:-s MINIFY_HTML=0"
            # And the UI font its text is drawn in.
            ${SR_WEB_UI_FONT_LINK_OPTION}
    )
endif()
