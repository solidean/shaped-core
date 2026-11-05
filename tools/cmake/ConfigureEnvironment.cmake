# The Windows developer environment a build directory was first configured in, which every later configure must share.
# Included before project(): what project() detects is the point.
#
# **CMake re-detects tools at every configure, from PATH and the environment, while the cache keeps what it found first.**
# A configure from a plain shell over a build made inside VsDevCmd pairs the two: under clang-cl the .rc rule follows
# whether `rc` is on PATH, so a cached rc.exe got the llvm-rc rule, rejected its input, and `cmake -E cmake_llvm_rc`
# deadlocked on the error text, hanging SDL3's version.rc with no output.
# The root CMakeLists seeds that one choice from the cache; this catches the next one before it costs a hung build.
#
# **A developer environment that appears or disappears is an error**, since nothing then says what was re-detected.
# **A toolset or SDK version that changes inside one is a warning**, and the new versions are recorded: Visual Studio
# updates move them, and ninja's header dependencies rebuild what the new headers change.
# dev.py enters the developer environment itself, so `uv run dev.py ...` always configures inside one.
# A build directory configured outside one, an emscripten or an android build, stays outside for life the same way.

if(NOT CMAKE_HOST_WIN32 OR NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    return()
endif()

set(_sc_env_present "$ENV{VSCMD_VER}")
set(_sc_env_versions "VS $ENV{VSCMD_VER}, MSVC tools $ENV{VCToolsVersion}, Windows SDK $ENV{WindowsSDKVersion}")
string(REPLACE "\\" "" _sc_env_versions "${_sc_env_versions}")
if(_sc_env_present)
    set(_sc_env_now "inside the developer environment (${_sc_env_versions})")
else()
    set(_sc_env_now "outside the developer environment")
endif()

if(NOT DEFINED SC_CONFIGURE_ENVIRONMENT_PRESENT)
    set(SC_CONFIGURE_ENVIRONMENT_PRESENT "${_sc_env_present}" CACHE INTERNAL "VSCMD_VER of the first configure, empty outside VsDevCmd")
    set(SC_CONFIGURE_ENVIRONMENT "${_sc_env_now}" CACHE INTERNAL "The developer environment the first configure ran in")
elseif((NOT SC_CONFIGURE_ENVIRONMENT_PRESENT STREQUAL "" AND _sc_env_present STREQUAL "")
       OR (SC_CONFIGURE_ENVIRONMENT_PRESENT STREQUAL "" AND NOT _sc_env_present STREQUAL ""))
    message(FATAL_ERROR
        "This build directory was configured ${SC_CONFIGURE_ENVIRONMENT}, and is now being configured ${_sc_env_now}.\n"
        "CMake would re-detect tools from this environment and pair them with what the cache holds from the other one, "
        "which has hung the build before.\n"
        "Configure through `uv run dev.py` (it enters the developer environment itself), or from the same kind of shell "
        "as the first configure.\n"
        "A build directory that is to change environments for good starts fresh: delete ${CMAKE_BINARY_DIR}.")
elseif(NOT SC_CONFIGURE_ENVIRONMENT STREQUAL _sc_env_now)
    message(WARNING
        "The developer environment changed since this build directory was configured: it was ${SC_CONFIGURE_ENVIRONMENT}, "
        "and is ${_sc_env_now}.\n"
        "The new versions are recorded; a build that misbehaves after this starts fresh from an empty build directory.")
    set(SC_CONFIGURE_ENVIRONMENT "${_sc_env_now}" CACHE INTERNAL "The developer environment the first configure ran in")
endif()

unset(_sc_env_present)
unset(_sc_env_versions)
unset(_sc_env_now)
