cmake_minimum_required(VERSION 3.28)

# NRD (NVIDIA Real-Time Denoisers): the split-signal denoise member behind sr::nrd_denoise_routine.
#
# Not committed, and — like DLSS — NOT fetched for you: the NVIDIA RTX SDKs license names the NRD SDK, and accepting
# one is a person's decision rather than a build's.
# fetch-nrd.py downloads the pinned sources when someone runs it; nothing in dev.py does.
#
# Built rather than unpacked. NRD ships no prebuilt package: its CMake compiles 31 HLSL entry points into embedded
# DXIL and produces a static library, which takes about ten seconds.
#
# Unlike DLSS it needs no native scope and no vendor runtime at all.
# NRD computes a LIST of dispatches and hands it back; shaped-rendering executes them through sg, which is what makes
# this the one vendor member that runs on any adapter.
set(NRD_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)

if(NOT EXISTS "${NRD_INSTALL}/pin.txt")
    message(FATAL_ERROR
        "NRD is not installed (${NRD_INSTALL} is missing). Fetch it once with:\n"
        "    uv run extern/nrd/fetch-nrd.py\n"
        "Nothing fetches it for you — see extern/nrd/dependency.yml for why.")
endif()

# NRD's build compiles its own shaders, which needs a DXC executable.
# extern/dxc already pins one, and pointing ShaderMake at it is the whole reason that pin now takes dxc.exe: left to
# itself ShaderMake DOWNLOADS a DXC at configure time, which would put a second, unpinned compiler in a build whose
# point is that every dependency is pinned — and would make configure need the network.
set(_nrd_dxc "${CMAKE_SOURCE_DIR}/extern/dxc/.install/bin/dxc.exe")
if(NOT WIN32 OR NOT EXISTS "${_nrd_dxc}")
    message(STATUS "nrd: no pinned dxc.exe to compile its shaders with — skipping, so sr::nrd_denoise_routine reports unsupported.")
    return()
endif()

# Pre-seeded rather than fetched: NRD's CMake declares both of these with FetchContent and a URL.
# A populated SOURCE_DIR makes FetchContent use what is there and download nothing, so the versions are ours to pin
# and configure stays offline.
set(FETCHCONTENT_SOURCE_DIR_SHADERMAKE "${NRD_INSTALL}/ShaderMake" CACHE PATH "" FORCE)
set(FETCHCONTENT_SOURCE_DIR_MATHLIB "${NRD_INSTALL}/MathLib" CACHE PATH "" FORCE)

# What NRD embeds. DXIL only: the vulkan backend has no native scope and no denoise member yet, and DXBC would need
# FXC from a Windows SDK we do not otherwise require.
set(NRD_EMBEDS_DXIL_SHADERS ON CACHE BOOL "" FORCE)
set(NRD_EMBEDS_DXBC_SHADERS OFF CACHE BOOL "" FORCE)
set(NRD_EMBEDS_SPIRV_SHADERS OFF CACHE BOOL "" FORCE)

# A static library, linked PRIVATE by shaped-rendering: nothing of NRD's reaches one of our public headers.
set(NRD_STATIC_LIBRARY ON CACHE BOOL "" FORCE)

# NRI is NVIDIA's own render abstraction, and we have sg — the integration layer that would use it is the one thing
# we are replacing.
set(NRD_NRI OFF CACHE BOOL "" FORCE)

# ShaderMake's own compiler discovery, all off: we hand it the one compiler we pin.
set(SHADERMAKE_FIND_COMPILERS OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_FIND_DXC OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_FIND_FXC OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_FIND_DXC_VK OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_FIND_SLANG OFF CACHE BOOL "" FORCE)
set(SHADERMAKE_DXC_PATH "${_nrd_dxc}" CACHE INTERNAL "" FORCE)

# Hides the tool from the parent project's IDE and keeps its shaders out of a configuration switch's rebuild.
set(SHADERMAKE_TOOL ON CACHE BOOL "" FORCE)

add_subdirectory("${NRD_INSTALL}/NRD" "${CMAKE_CURRENT_BINARY_DIR}/nrd" EXCLUDE_FROM_ALL)

# NRD's warnings are NRD's business, and it builds with -Werror of its own.
# A consumer's stricter settings must not turn an upstream's clean build into a broken one.
if(TARGET NRD)
    target_compile_options(NRD PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang,GNU>:-w> $<$<CXX_COMPILER_ID:MSVC>:/w>)
endif()
