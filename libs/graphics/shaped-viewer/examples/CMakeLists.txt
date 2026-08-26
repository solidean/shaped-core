# shaped-viewer examples: one binary per file, each demonstrating the viewer in practice.
# Run one with `uv run dev.py example shaped-viewer/hello-cube`; see docs/guides/examples.md.
#
# Gated rather than broken: an example opens a real window and path-traces through DXC-compiled shaders, which today
# means dx12 + DXR on Windows. Every CI leg builds all targets, wasm and cross-compile included, so an ungated
# graphical example would break builds that could never have run it.
if(NOT (WIN32 AND SC_HAS_DXC_COMPILER AND SC_HAS_SDL3 AND TARGET shaped-graphics-dx12))
    return()
endif()

sc_add_single_file_examples(shaped-viewer
    FILES
    hello-cube.cc
    LINK shaped-viewer
    PCH SG NEXUS
)
