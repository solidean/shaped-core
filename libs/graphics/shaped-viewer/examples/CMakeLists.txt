# shaped-viewer examples: one binary per file, each demonstrating the viewer in practice.
# Run one with `uv run dev.py example shaped-viewer/hello-cube`; see docs/guides/examples.md.
#
# Gated rather than broken: an example path-traces on the viewer's default context, which is dx12 on Windows, and its
# SGL reaches DXIL through DXC.
# Every CI leg builds all targets, wasm and cross-compile included, so an ungated graphical example would break builds that
# could never have run it.
#
# SDL3 is deliberately NOT part of the gate. A capture run brings no window system up at all, so an example is
# capturable wherever the tracing stack exists; without SDL3 only the interactive path is unavailable, and that one
# already fails cleanly.
if(NOT (WIN32 AND SC_HAS_DXC_COMPILER AND TARGET shaped-graphics-dx12))
    return()
endif()

# The per-example settings below name the example's target, which does not exist when examples are off.
if(NOT SC_BUILD_EXAMPLES)
    return()
endif()

# Every example here, by stem: each is <stem>.cc and the target shaped-viewer-<stem>-example.
set(_sv_example_stems hello-cube load-asset openpbr-spheres mesh-structure mesh-structure-dense quadric-gallery quadric-arrows lights light-stage drawings decals bistro raster tg-objects)

# Its own gate passed, so the question left is which backend the build was ASKED for.
# The viewer's default context is dx12's, so a build set to vulkan gets targets that say so, though the tracer itself runs there.
sc_resolve_example_backend(_sv_backend _sv_mode SUPPORTS dx12)
if(_sv_mode STREQUAL "none")
    return()
endif()
if(_sv_mode STREQUAL "stub")
    foreach(_stem IN LISTS _sv_example_stems)
        sc_add_example_backend_stub(shaped-viewer-${_stem}-example "${SC_EXAMPLE_BACKEND}" SUPPORTS dx12)
    endforeach()
    return()
endif()

list(TRANSFORM _sv_example_stems APPEND ".cc" OUTPUT_VARIABLE _sv_example_sources)
sc_add_single_file_examples(shaped-viewer
    FILES
    ${_sv_example_sources}
    LINK shaped-viewer
    PCH SG NEXUS
)

# NGX loads Ray Reconstruction from a DLL beside the executable, so without it staged `automatic` falls back to a lesser denoiser.
if(SC_DLSS_RUNTIME)
    foreach(_stem IN LISTS _sv_example_stems)
        sc_stage_runtime_dlls(shaped-viewer-${_stem}-example DLLS "${SC_DLSS_RUNTIME}" TAG dlss-${_stem})
    endforeach()
endif()

# Bistro is fetched on request into extern/bistro/.install, so the example names where it lands rather than what it
# contains, and reports a missing scene at run time: a fetch after the build then needs no reconfigure.
cmake_path(SET _sv_bistro_dir NORMALIZE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../extern/bistro/.install")
target_compile_definitions(shaped-viewer-bistro-example PRIVATE SV_BISTRO_DIR="${_sv_bistro_dir}")
