# A mini editor over a versioned document: vdoc for the model, vdoc-file for persistence, sg/sr for the picture.
#
# Windows-only in practice, and gated rather than broken: it creates a dx12 context directly and compiles HLSL through
# DXC, so a wasm or cross-compile CI leg (which builds every target) must not see this target at all.
# libs/graphics/shaped-rendering/CMakeLists.txt gates its own end-to-end imgui test exactly this way.
if(NOT (WIN32 AND SC_HAS_DXC_COMPILER AND SC_HAS_SDL3 AND TARGET shaped-graphics-dx12))
    return()
endif()

# One binary, two EXAMPLEs over shared setup — the case a single-file example cannot show.
sc_add_example(vdoc-cube-editor-example
    SOURCES
    cube_document.cc
    cube_app.cc
    cube_renderer.cc
    example-viewer.cc
    example-editor.cc
    LINK
    versioned-document
    versioned-document-file
    shaped-rendering
    shaped-graphics-dx12
)

# The package emits C++ descriptors and embeds the sources; compilation happens at runtime through slib.
# On Windows it also stages dxcompiler.dll and dxil.dll beside the executable, because this target is an EXECUTABLE.
sc_add_shader_package(
    TARGET     vdoc-cube-editor-example
    NAME       cube_shaders
    NAMESPACE  cube_editor::shaders
    SOURCE_DIR shaders
    LANGUAGE   hlsl
    SHADERS
        cube.hlsl:vertex:main_vs
        cube.hlsl:fragment:main_ps
)
