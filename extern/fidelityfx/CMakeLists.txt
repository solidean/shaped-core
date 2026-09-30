cmake_minimum_required(VERSION 3.28)

# AMD FSR 3.1's upscaler, from the FidelityFX SDK: its host C++ as an object library, and its HLSL passes as sources for
# shaped-rendering's shader package.
#
# AMD's host code plans each frame and reaches the GPU only through its `FfxInterface` table, which shaped-rendering
# implements over sg; nothing here talks to a graphics API.
# It leaves a few functions unresolved on purpose — the permutation lookup, message printing, assert reporting — and
# shaped-rendering defines them, which is why this is an OBJECT library its objects join rather than an archive linked
# after it.
set(FIDELITYFX_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)
set(FIDELITYFX_ROOT ${FIDELITYFX_INSTALL}/Kits/FidelityFX)

if(NOT EXISTS "${FIDELITYFX_INSTALL}/pin.txt")
    message(FATAL_ERROR
        "FidelityFX is not installed (${FIDELITYFX_INSTALL} is missing). Fetch it once with:\n"
        "    uv run extern/fidelityfx/fetch-fidelityfx.py\n"
        "dev.py does this on demand; see tools/dev/lib/pipeline/prereqs.py.")
endif()

# The host code includes a watermark and a git hash from `amdinternal/`, which the public SDK does not carry.
# The stand-ins go where the host code's relative includes look, and configure refreshes them.
file(COPY "${CMAKE_CURRENT_SOURCE_DIR}/shim/amdinternal" DESTINATION "${FIDELITYFX_ROOT}")

add_library(fidelityfx-fsr3 OBJECT
    ${FIDELITYFX_ROOT}/upscalers/fsr3/internal/ffx_fsr3upscaler.cpp
    ${FIDELITYFX_ROOT}/api/internal/ffx_object_management.cpp
)

# Consumers include the SDK by its own tree, `<upscalers/fsr3/include/ffx_fsr3upscaler.h>`.
target_include_directories(fidelityfx-fsr3 SYSTEM PUBLIC "${FIDELITYFX_ROOT}")
target_compile_features(fidelityfx-fsr3 PUBLIC cxx_std_20)

# AMD's warnings are AMD's business.
# A consumer's stricter settings must not turn an upstream's clean build into a broken one.
target_compile_options(fidelityfx-fsr3 PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-w> $<$<CXX_COMPILER_ID:MSVC>:/w>)

# The HLSL the passes are built from, flattened into one directory for shaped-rendering's shader package.
#
# TEMPORARY, and written into the source tree for the reason extern/nrd's copy is: sc_add_shader_package resolves every
# #include under one SOURCE_DIR and embeds what it finds there, and FSR's sources span three trees.
# slib's resolver also looks for a nested include from the entry's own roots rather than from its includer, so the
# includes are rewritten to bare file names, which is sound because every basename in the set is unique.
# The gap this works around is "Package-level include paths" under Deferred in libs/graphics/shaped-shader-library/docs/structure.md.
# The copy is gitignored, and configure refreshes it, so it can never be staler than the fetched sources.
set(_fsr_shader_dir "${CMAKE_SOURCE_DIR}/libs/graphics/shaped-rendering/shaders/fsr3")
file(GLOB _fsr_shader_sources
    "${FIDELITYFX_ROOT}/api/internal/gpu/*.h"
    "${FIDELITYFX_ROOT}/upscalers/fsr3/include/gpu/fsr1/*.h"
    "${FIDELITYFX_ROOT}/upscalers/fsr3/include/gpu/spd/*.h"
    "${FIDELITYFX_ROOT}/upscalers/fsr3/include/gpu/fsr3upscaler/*.h"
    "${FIDELITYFX_ROOT}/upscalers/fsr3/internal/shaders/ffx_fsr3upscaler_*_pass.hlsl")
file(MAKE_DIRECTORY "${_fsr_shader_dir}")
foreach(_source IN LISTS _fsr_shader_sources)
    get_filename_component(_name "${_source}" NAME)
    file(READ "${_source}" _text)
    string(REGEX REPLACE "#include \"[^\"]*/([^\"/]+)\"" "#include \"\\1\"" _text "${_text}")
    # TEMPORARY: AMD numbers each register class from zero, and SPIR-V maps `t0`, `u0`, `b0` and `s0` all onto binding 0.
    # Under __spirv__ each class gets its own decimal prefix instead — b stays `N`, t becomes `1N`, u `2N`, s `3N` — so
    # the bindings are distinct while no constant buffer index reaches 10.
    # The real fix is per-class binding shifts (DXC's `-fvk-*-shift`) that a shader package can ask slib for.
    if(_name STREQUAL "ffx_core_hlsl.h")
        foreach(_class IN ITEMS "SRV;t;1" "UAV;u;2" "SAMPLER;s;3")
            list(GET _class 0 _macro)
            list(GET _class 1 _reg)
            list(GET _class 2 _prefix)
            set(_line "#define DECLARE_${_macro}_REGISTER(regIndex) ${_reg}##regIndex")
            if(_macro STREQUAL "SAMPLER")
                set(_line "#define DECLARE_SAMPLER_REGISTER(regIndex)  s##regIndex")
            endif()
            string(FIND "${_text}" "${_line}" _at)
            if(_at EQUAL -1)
                message(FATAL_ERROR "fidelityfx: ffx_core_hlsl.h no longer declares `${_line}`; the SPIR-V binding patch needs updating")
            endif()
            string(REPLACE "${_line}"
                "#if defined(__spirv__)\n#define DECLARE_${_macro}_REGISTER(regIndex) ${_reg}${_prefix}##regIndex\n#else\n${_line}\n#endif"
                _text "${_text}")
        endforeach()
    endif()
    # Through a staging file, so an unchanged source keeps its timestamp and regenerates nothing.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/fsr3-staging/${_name}" "${_text}")
    file(COPY_FILE "${CMAKE_CURRENT_BINARY_DIR}/fsr3-staging/${_name}" "${_fsr_shader_dir}/${_name}" ONLY_IF_DIFFERENT)
endforeach()
