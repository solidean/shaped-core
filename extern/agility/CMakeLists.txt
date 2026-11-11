cmake_minimum_required(VERSION 3.28)

# Microsoft's D3D12 Agility SDK from NuGet: a D3D12 runtime newer than the one Windows carries.
#
# Fetched ON REQUEST by fetch-agility.py (`uv run extern/agility/fetch-agility.py --accept-license`), never by a
# configure; dependency.yml says why.
# Nothing links a library of it: the dx12 backend compiles against its headers and loads its D3D12Core.dll at run time,
# through ID3D12SDKConfiguration1::CreateDeviceFactory, from the directory this publishes.
# AGILITY_INSTALL is the parent's choice: this directory's retail install, or extern/agility-preview's under SC_AGILITY_PREVIEW.
if(NOT AGILITY_INSTALL)
    set(AGILITY_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)
endif()

if(NOT EXISTS "${AGILITY_INSTALL}/bin/D3D12Core.dll" OR NOT EXISTS "${AGILITY_INSTALL}/include/d3d12.h")
    message(STATUS "agility: ${AGILITY_INSTALL} holds no runtime or headers — re-run its fetch with `--accept-license --force`; dx12 keeps the system runtime.")
    return()
endif()

add_library(agility INTERFACE)
# Ahead of the Windows SDK's, whose d3d12.h is older: every translation unit that includes it sees one version.
target_include_directories(agility SYSTEM BEFORE INTERFACE "${AGILITY_INSTALL}/include")
# Where D3D12Core.dll is, which a backend without one staged beside its executable loads from.
set_target_properties(agility PROPERTIES SC_AGILITY_RUNTIME_DIR "${AGILITY_INSTALL}/bin")
