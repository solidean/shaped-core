# The Windows developer environment a build directory was first configured in, which every later configure must share.
# Included before project(), by shaped-core's root CMakeLists and by a project consuming shaped-core in its own root CMakeLists:
# what project() detects is the point.
# Included only from shaped-core, after the consumer's project(), it still refuses a switch before anything is generated.
#
# **CMake re-detects tools at every configure, from PATH and the environment, while the cache keeps what it found first.**
# A configure from a plain shell over a build made inside VsDevCmd pairs the two: under clang-cl the .rc rule follows
# whether `rc` is on PATH, so a cached rc.exe got the llvm-rc rule, and `cmake -E cmake_llvm_rc` handed rc.exe
# hangs with no output, in any environment, instead of failing: SDL3's version.rc hangs the build.
# Seeding that one choice from the cache keeps the rule and the compiler one pair; the check below catches the next one.
#
# **A developer environment that appears or disappears is an error**, since nothing then says what was re-detected.
# **A toolset or SDK version that changes inside one is a warning**, and the new versions are recorded: Visual Studio
# updates move them, and ninja's header dependencies rebuild what the new headers change.
# dev.py enters the developer environment itself, so `uv run dev.py ...` always configures inside one.
# A build directory configured outside one, an emscripten or an android build, stays outside for life the same way.

include_guard(GLOBAL)

if(NOT CMAKE_HOST_WIN32)
    return()
endif()

# Effective only before the first project(); after it, the rule is chosen and the check below is what protects the build.
if(CMAKE_RC_COMPILER AND NOT CMAKE_RC_COMPILER_INIT)
    set(CMAKE_RC_COMPILER_INIT "${CMAKE_RC_COMPILER}")
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
        "Configure through `uv run dev.py` (it enters the developer environment itself; --preset picks any build preset), "
        "or from the same kind of shell as the first configure.\n"
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
