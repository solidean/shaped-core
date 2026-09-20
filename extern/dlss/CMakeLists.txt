cmake_minimum_required(VERSION 3.28)

# NVIDIA DLSS: the SDK behind shaped-rendering's sr::dlss_rr_routine (Ray Reconstruction).
#
# Not committed, and unlike every other fetched dependency NOT fetched for you — the license is NVIDIA's own rather
# than an OSI one, so obtaining it is a person's decision.
# fetch-dlss.py downloads the pinned files into .install/ when someone runs it; nothing in dev.py does.
# Without it this directory is never added, SR_HAS_DLSS is 0, and `sr::denoise_method::dlss_rr` reports `unsupported`.
#
# An IMPORTED target rather than a build: what arrives is headers plus a prebuilt import library, so there is nothing
# here to compile.
set(DLSS_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)

if(NOT EXISTS "${DLSS_INSTALL}/pin.txt")
    message(FATAL_ERROR
        "DLSS is not installed (${DLSS_INSTALL} is missing). Fetch it once with:\n"
        "    uv run extern/dlss/fetch-dlss.py\n"
        "Nothing fetches it for you — see extern/dlss/dependency.yml for why.")
endif()

# Windows x86_64 only, which is the one platform this integration targets.
# A vendor member needs sg's native scope to hand foreign code a command list, and dx12 is the only backend with one.
if(NOT WIN32 OR NOT SC_ARCH_X64)
    message(STATUS "dlss: not a Windows x64 build — skipping, so sr::dlss_rr_routine reports unsupported.")
    return()
endif()

set(_dlss_lib "${DLSS_INSTALL}/lib/Windows_x86_64/x64/nvsdk_ngx_d.lib")
set(_dlss_runtime "${DLSS_INSTALL}/lib/Windows_x86_64/rel/nvngx_dlssd.dll")

if(NOT EXISTS "${_dlss_lib}" OR NOT EXISTS "${_dlss_runtime}")
    message(STATUS "dlss: the install is missing its library or runtime — skipping.")
    return()
endif()

add_library(dlss STATIC IMPORTED GLOBAL)
set_target_properties(dlss PROPERTIES IMPORTED_LOCATION "${_dlss_lib}")
target_include_directories(dlss INTERFACE "${DLSS_INSTALL}/include")

# NGX links against these itself; naming them here keeps every consumer from repeating the list.
target_link_libraries(dlss INTERFACE d3d12 dxgi advapi32 shlwapi)

# Where the runtime lives, for the consumer that has to stage it next to a binary.
# A property rather than a variable, so it travels with the target rather than with directory scope.
set_target_properties(dlss PROPERTIES SC_DLSS_RUNTIME "${_dlss_runtime}")
