cmake_minimum_required(VERSION 3.28)

# DirectX Shader Compiler (DXC). Unlike the other extern/ deps, DXC is neither vendored as
# source nor built from source (its from-source build is LLVM-scale). Instead download-dxc.py
# fetches the pinned official release binaries for the host arch into .install/ (dev.py runs it
# on demand; see tools/dev/lib/pipeline/prereqs.py). Here we only wrap those prebuilt artifacts
# as an IMPORTED target named `dxc`.
set(DXC_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)

if(NOT EXISTS "${DXC_INSTALL}/pin.txt")
    message(FATAL_ERROR
        "DXC is not installed (${DXC_INSTALL} is missing). Fetch it once with:\n"
        "    uv run extern/dxc/download-dxc.py\n"
        "dev.py does this automatically on Windows; see tools/dev/lib/pipeline/prereqs.py.")
endif()

# SHARED + IMPORTED so consumers link the prebuilt library. GLOBAL so the graphics library (a sibling
# add_subdirectory) can link it.
#
# The two platforms differ in what there is to link.
# Windows ships a DLL plus an import lib; Linux ships a bare .so with no import library and no SONAME, so
# IMPORTED_NO_SONAME is required — without it the link records the full build path.
add_library(dxc SHARED IMPORTED GLOBAL)
if(WIN32)
    set_target_properties(dxc PROPERTIES
        IMPORTED_LOCATION "${DXC_INSTALL}/bin/dxcompiler.dll"
        IMPORTED_IMPLIB "${DXC_INSTALL}/lib/dxcompiler.lib")
else()
    set_target_properties(dxc PROPERTIES
        IMPORTED_LOCATION "${DXC_INSTALL}/lib/libdxcompiler.so"
        IMPORTED_NO_SONAME TRUE)
endif()

# SYSTEM so dxcapi.h does not trip the consuming TU's warnings.
target_include_directories(dxc SYSTEM INTERFACE "${DXC_INSTALL}/include")

# Runtime DLLs a consuming binary must have beside it: dxcompiler + its dxil.dll signer (so the
# emitted DXIL is signed and dx12 accepts it). $<TARGET_RUNTIME_DLLS> only sees dxcompiler (the
# imported target), so we expose the full list for consumers to stage.
#
# Empty off Windows: there is no signer to stage (SPIR-V needs no signing), and the loader finds the .so through the
# imported target's RPATH rather than a copy beside the binary.
if(WIN32)
    set(DXC_RUNTIME_DLLS "${DXC_INSTALL}/bin/dxcompiler.dll" "${DXC_INSTALL}/bin/dxil.dll"
        CACHE INTERNAL "DXC runtime DLLs to copy next to consuming binaries")
else()
    set(DXC_RUNTIME_DLLS "" CACHE INTERNAL "DXC runtime DLLs to copy next to consuming binaries")
endif()
