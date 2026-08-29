# shaped-viewer examples: one binary per file, each demonstrating the viewer in practice.
# Run one with `uv run dev.py example shaped-viewer/hello-cube`; see docs/guides/examples.md.
#
# Gated rather than broken: an example path-traces through DXC-compiled shaders, which today means dx12 + DXR on
# Windows. Every CI leg builds all targets, wasm and cross-compile included, so an ungated graphical example would
# break builds that could never have run it.
#
# SDL3 is deliberately NOT part of the gate. A capture run brings no window system up at all, so an example is
# capturable wherever the tracing stack exists; without SDL3 only the interactive path is unavailable, and that one
# already fails cleanly.
if(NOT (WIN32 AND SC_HAS_DXC_COMPILER AND TARGET shaped-graphics-dx12))
    return()
endif()

sc_add_single_file_examples(shaped-viewer
    FILES
    hello-cube.cc
    openpbr-spheres.cc
    LINK shaped-viewer
    PCH SG NEXUS
)
