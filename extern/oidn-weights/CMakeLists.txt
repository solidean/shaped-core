cmake_minimum_required(VERSION 3.28)

# The trained Open Image Denoise networks, without the library that usually carries them.
#
# There is nothing to compile and nothing to link: what arrives is two files and 2.5 MB of weights that shaped-rendering
# reads at run time and uploads to the GPU, because it runs the network in its own compute shaders.
# So this exposes a PATH rather than a target.
set(OIDN_WEIGHTS_INSTALL ${CMAKE_CURRENT_SOURCE_DIR}/.install)

if(NOT EXISTS "${OIDN_WEIGHTS_INSTALL}/pin.txt")
    message(FATAL_ERROR
        "The OIDN weights are not installed (${OIDN_WEIGHTS_INSTALL} is missing). Fetch them once with:\n"
        "    uv run extern/oidn-weights/fetch-oidn-weights.py\n"
        "dev.py does this on demand; see tools/dev/lib/pipeline/prereqs.py.")
endif()

# Cached so shaped-rendering, a sibling add_subdirectory, can compile the path in as SR_OIDN_WEIGHTS_DIR.
set(OIDN_WEIGHTS_DIR "${OIDN_WEIGHTS_INSTALL}" CACHE INTERNAL "directory holding the pinned OIDN .tza weights")
