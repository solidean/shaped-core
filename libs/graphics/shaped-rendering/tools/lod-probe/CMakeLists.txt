# lod-probe: which mip level a sampler picks, per GPU, per backend — and the gap between dx12 and vulkan.
# A nexus binary so its CPU-side tests live beside the command; see lod_probe.cc for the command line.
#
# Both backends are linked into the one binary, so the two measurements come from the same build and the same process.
# SGL reaches both through DXC, which is why there is nothing to build without it.

set(_lod_probe_backends "")
set(_lod_probe_has_dx12 0)
set(_lod_probe_has_vulkan 0)
if(TARGET shaped-graphics-dx12)
    list(APPEND _lod_probe_backends shaped-graphics-dx12)
    set(_lod_probe_has_dx12 1)
endif()
if(TARGET shaped-graphics-vulkan)
    list(APPEND _lod_probe_backends shaped-graphics-vulkan)
    set(_lod_probe_has_vulkan 1)
endif()
if(NOT _lod_probe_backends OR NOT SC_HAS_DXC_COMPILER)
    return()
endif()

add_executable(lod-probe
    main.cc
    lod_probe.cc
    probe.cc
    report.cc
    floor.cc
    lod-probe-test.cc
)
sc_nexus_binary(lod-probe KINDS tool tests)
target_link_libraries(lod-probe PRIVATE shaped-rendering nexus ${_lod_probe_backends})

target_compile_definitions(lod-probe PRIVATE
    LOD_PROBE_HAS_DX12=${_lod_probe_has_dx12}
    LOD_PROBE_HAS_VULKAN=${_lod_probe_has_vulkan}
)

sc_add_shader_package(
    TARGET     lod-probe
    NAME       lod_probe_shaders
    NAMESPACE  shaders
    SOURCE_DIR shaders
    LANGUAGE   sgl
    SHADERS
        lod_probe.sgl:*
        floor.sgl:*
)

sc_stage_runtime_dlls(lod-probe DLLS "$<TARGET_RUNTIME_DLLS:lod-probe>")

# What a copy handed to someone else needs beside the exe: every runtime DLL, DXC's included.
file(GENERATE OUTPUT "$<TARGET_FILE_DIR:lod-probe>/lod-probe.runtime-dlls"
    CONTENT "$<JOIN:$<TARGET_RUNTIME_DLLS:lod-probe>;${DXC_RUNTIME_DLLS},\n>\n")
