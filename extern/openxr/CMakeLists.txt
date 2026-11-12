cmake_minimum_required(VERSION 3.28)

# The Khronos OpenXR loader, prebuilt: what an OpenXR application links to reach the active runtime.
# Not committed; fetch-openxr.py downloads the pinned release into .install/, and dev.py runs it per configure.
# Windows on x64 only for now, the one loader fetch-openxr.py takes from the release's package.
set(OPENXR_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)

if(NOT WIN32 OR NOT SC_ARCH_X64 OR NOT EXISTS "${OPENXR_INSTALL}/bin/openxr_loader.dll")
    message(STATUS "openxr: no loader installed (uv run extern/openxr/fetch-openxr.py) — the xr examples are not built")
    return()
endif()

add_library(openxr_loader SHARED IMPORTED GLOBAL)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION "${OPENXR_INSTALL}/bin/openxr_loader.dll"
    IMPORTED_IMPLIB "${OPENXR_INSTALL}/lib/openxr_loader.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENXR_INSTALL}/include"
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${OPENXR_INSTALL}/include")
