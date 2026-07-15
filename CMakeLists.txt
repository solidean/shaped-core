# Minimum versions and the rationale: docs/requirements.md
cmake_minimum_required(VERSION 3.28)
project(shaped-core LANGUAGES CXX)

# Lock in C++23 for the whole repo.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Build gating. Both default ON for a top-level build (the normal dev flow) and OFF when
# shaped-core is consumed via add_subdirectory, where a dependent wants the libraries but not
# our test and tool binaries. Must precede add_subdirectory(extern): the tools' externals gate
# on SC_BUILD_TOOLS.
option(SC_BUILD_TESTS "Build the per-library *-test executables" ${PROJECT_IS_TOP_LEVEL})
option(SC_BUILD_TOOLS "Build the developer tools in tools/" ${PROJECT_IS_TOP_LEVEL})

# Repo-wide build configuration helpers live in tools/cmake/.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake")

# Apply repo-wide build configuration before add_subdirectory so every target is
# instrumented. Each is a no-op unless its driving option/preset is active.
include(Coverage)     # SC_COVERAGE  — LLVM source-based test coverage
include(Sanitizers)   # SANITIZE     — ASan/UBSan wiring (clang only)
include(PGO)          # SC_PGO_*     — IR-based profile-guided optimization (clang only)
include(Emscripten)   # EMSCRIPTEN   — WASM feature + exception-mode knobs

# RelWithDebInfo = Release codegen + assertions + debug info. MSVC/clang-cl default this config to /Ob1
# (inline only functions explicitly marked inline) — a misleading dev default: /O2 is otherwise identical to
# Release, but the inlining downgrade alone leaves heavily-inlined code several times slower (e.g. xxHash's
# short-key hash path was ~5-11x slower under /Ob1). Promote to /Ob2 so RelWithDebInfo matches Release's
# inlining; /Zi debug info and our CC_ASSERTs are untouched. Applied to every target (C and C++) defined below.
if (MSVC)
    add_compile_options($<$<CONFIG:RelWithDebInfo>:/Ob2>)
endif()

# Vendored third-party dependencies (mimalloc, xxHash, ...). Defined before the
# libs so their targets exist when clean-core links them.
add_subdirectory(extern)

# Browser test-runner helper (sc_add_nexus_web_runner). Included before the libs so both clean-core and
# nexus can build an Emscripten `*-web.html` runner; a no-op on native toolchains.
include(libs/base/nexus/cmake/NexusWebRunner.cmake)

# clean-core must come first: nexus depends on it.
add_subdirectory(libs/base/clean-core)
add_subdirectory(libs/base/nexus)
add_subdirectory(libs/base/typed-geometry)

# graphics stack: shaped-graphics (sg) -> shaped-rendering (sr) -> shaped-viewer (sv).
# Added in dependency order; sg pulls in its per-backend static libraries itself.
add_subdirectory(libs/graphics/shaped-graphics)
add_subdirectory(libs/graphics/shaped-rendering)
add_subdirectory(libs/graphics/shaped-viewer)

# shaped-shader-compiler-dxc (ssc): HLSL -> sg::compiled_shader via DXC. Windows-only, and only when
# DXC has been built (extern/dxc adds the `dxc` target). Run `uv run extern/dxc/build-dxc.py` (dev.py
# does this automatically) to enable it.
if(WIN32 AND TARGET dxc)
    add_subdirectory(libs/graphics/shaped-shader-compiler-dxc)
endif()

# Developer tools. instruction-tracer needs the Win32 debug API, dbghelp and the fetched Zydis
# decoder (extern/zydis); it gates itself on the `zydis` target existing, so a checkout that has
# not fetched it still configures.
if(SC_BUILD_TOOLS AND WIN32 AND TARGET zydis)
    add_subdirectory(tools/instruction-tracer)
endif()

# Generate the browser test-runner pages once every *-web module has been registered (Emscripten only):
# one page per library plus the aggregate tests-web.html. See cmake/NexusWebRunner.cmake.
sc_finalize_web_runners()
