cmake_minimum_required(VERSION 3.28)

# Intel Open Image Denoise (OIDN), the library: the reference the shaped-rendering-test oracle compares sr::denoise_method::oidn against.
# Nothing we ship links it; the member itself runs the network from extern/oidn-weights in our own compute shaders.
#
# Fetched ON REQUEST: no configure step runs fetch-oidn.py, so .install/ is normally absent and the oracle tests SKIP.
# `uv run extern/oidn/fetch-oidn.py` hydrates .install/, and the next configure picks it up.
#
# An IMPORTED target rather than a build: what arrives is headers plus prebuilt libraries, and building OIDN from
# source would drag in oneDNN and a training-weights pipeline.
#
# CPU ONLY: the install carries no CUDA, HIP or SYCL device module, because the oracle needs none.
set(OIDN_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)

if(NOT EXISTS "${OIDN_INSTALL}/pin.txt")
    message(FATAL_ERROR
        "OIDN is not installed (${OIDN_INSTALL} is missing). Fetch it once with:\n"
        "    uv run extern/oidn/fetch-oidn.py\n"
        "Nothing fetches it for you — see extern/oidn/dependency.yml for why.")
endif()

# The facade library is what a consumer links; the core beside it carries the trained weights and is loaded by it.
# GLOBAL so shaped-rendering-test, under a sibling add_subdirectory, can link it.
add_library(oidn SHARED IMPORTED GLOBAL)
if(WIN32)
    set_target_properties(oidn PROPERTIES
        IMPORTED_LOCATION "${OIDN_INSTALL}/bin/OpenImageDenoise.dll"
        IMPORTED_IMPLIB "${OIDN_INSTALL}/lib/OpenImageDenoise.lib")
else()
    # lib/ rather than bin/, and no IMPORTED_NO_SONAME: these carry a real SONAME (`libOpenImageDenoise.so.2`, or an
    # `@rpath` install name on macOS), so CMake records that and adds the directory to the binary's rpath.
    # Claiming they have none makes CMake link by full path and skip the rpath, which leaves the loader with a SONAME
    # it has nowhere to look for.
    set(_oidn_facade "${OIDN_INSTALL}/lib/libOpenImageDenoise.dylib")
    if(NOT APPLE)
        set(_oidn_facade "${OIDN_INSTALL}/lib/libOpenImageDenoise.so")
    endif()
    set_target_properties(oidn PROPERTIES IMPORTED_LOCATION "${_oidn_facade}")
endif()

# SYSTEM so oidn.h does not trip the consuming TU's warnings.
target_include_directories(oidn SYSTEM INTERFACE "${OIDN_INSTALL}/include")

# Every library a consuming binary must have beside it — a Windows concern only.
#
# There the facade alone is not enough and the failure is a run-time one: it loads the core and the device module by
# name out of its own directory, so a binary staged with only OpenImageDenoise.dll reports that no device came up.
# $<TARGET_RUNTIME_DLLS> sees only the imported target, so the full list travels here as DXC's does.
if(WIN32)
    file(GLOB _oidn_runtime "${OIDN_INSTALL}/bin/*.dll")
else()
    # Empty on purpose: nothing is staged beside a Unix binary, because each library finds its siblings through the
    # RPATH upstream built into it and the loader finds the first one through ours.
    set(_oidn_runtime "")
endif()
set(OIDN_RUNTIME_LIBRARIES "${_oidn_runtime}"
    CACHE INTERNAL "OIDN libraries to copy next to consuming binaries")
