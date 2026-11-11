cmake_minimum_required(VERSION 3.28)

# Microsoft's WARP from NuGet: a newer D3D12 software adapter than the one Windows ships.
#
# Fetched ON REQUEST by fetch-warp.py (`uv run extern/warp/fetch-warp.py`), never by a configure; dependency.yml says why.
# Nothing links it: a d3d10warp.dll beside an executable is loaded instead of the system one, so all this does is say
# where the DLL is, and shaped-graphics' sg_warp_test_binary stages it beside every WARP test binary.
set(WARP_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)

if(NOT EXISTS "${WARP_INSTALL}/bin/d3d10warp.dll")
    message(STATUS "warp: the install holds no d3d10warp.dll — re-run `uv run extern/warp/fetch-warp.py --force`; the tests keep Windows' own WARP.")
    return()
endif()

set(SC_PINNED_WARP_DLL "${WARP_INSTALL}/bin/d3d10warp.dll" CACHE INTERNAL "The pinned WARP the dx12 test binaries load")
