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

# SHARED + IMPORTED_LOCATION(dll)/IMPORTED_IMPLIB(lib) so consumers link the import lib. GLOBAL so
# the graphics library (a sibling add_subdirectory) can link it.
add_library(dxc SHARED IMPORTED GLOBAL)
set_target_properties(dxc PROPERTIES
    IMPORTED_LOCATION "${DXC_INSTALL}/bin/dxcompiler.dll"
    IMPORTED_IMPLIB "${DXC_INSTALL}/lib/dxcompiler.lib")

# SYSTEM so dxcapi.h does not trip the consuming TU's warnings.
target_include_directories(dxc SYSTEM INTERFACE "${DXC_INSTALL}/include")

# Runtime DLLs a consuming binary must have beside it: dxcompiler + its dxil.dll signer (so the
# emitted DXIL is signed and dx12 accepts it). $<TARGET_RUNTIME_DLLS> only sees dxcompiler (the
# imported target), so we expose the full list for consumers to stage.
set(DXC_RUNTIME_DLLS "${DXC_INSTALL}/bin/dxcompiler.dll" "${DXC_INSTALL}/bin/dxil.dll"
    CACHE INTERNAL "DXC runtime DLLs to copy next to consuming binaries")
