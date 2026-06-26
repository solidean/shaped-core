# LLVM IR-based Profile-Guided Optimization (repo-wide). Included once from the root CMakeLists before
# add_subdirectory so the flags reach every target. A no-op unless one of the *-pgo-generate / *-pgo-use
# presets is active. This is IR-based PGO (-fprofile-generate / -fprofile-use), distinct from coverage's
# frontend instrumentation (-fprofile-instr-generate); both flag families are accepted by clang, AppleClang,
# and clang-cl. See docs/guides/pgo.md.

option(SC_PGO_GENERATE "Build instrumented for PGO profile generation (clang only)" OFF)
option(SC_PGO_USE "Build optimized using a collected PGO profile (clang only)" OFF)
set(SC_PGO_PROFILE "" CACHE FILEPATH "Merged .profdata for SC_PGO_USE (default: build/pgo/pgo.profdata)")

if(SC_PGO_GENERATE OR SC_PGO_USE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SC_PGO requires a Clang/LLVM toolchain (got ${CMAKE_CXX_COMPILER_ID})")
    endif()
endif()

if(SC_PGO_GENERATE)
    # Instrumented binaries honor LLVM_PROFILE_FILE and drop *.profraw at exit (same env hook coverage uses).
    add_compile_options(-fprofile-generate)
    add_link_options(-fprofile-generate)
endif()

if(SC_PGO_USE)
    set(_prof "${SC_PGO_PROFILE}")
    if(_prof STREQUAL "")
        set(_prof "${CMAKE_SOURCE_DIR}/build/pgo/pgo.profdata")
    endif()
    if(NOT EXISTS "${_prof}")
        message(FATAL_ERROR "SC_PGO_USE: profile not found at ${_prof} — run: uv run dev.py pgo train")
    endif()
    # The two -Wno-profile-instr-* silence benign "stale profile" warnings when sources drift from the profile.
    add_compile_options(-fprofile-use=${_prof} -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled)
    add_link_options(-fprofile-use=${_prof})
endif()
