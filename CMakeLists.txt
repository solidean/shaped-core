# Minimum versions and the rationale: docs/requirements.md
cmake_minimum_required(VERSION 3.28)
project(shaped-core LANGUAGES CXX)

# Lock in C++23 for the whole repo.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

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

# Generate the browser test-runner pages once every *-web module has been registered (Emscripten only):
# one page per library plus the aggregate tests-web.html. See cmake/NexusWebRunner.cmake.
sc_finalize_web_runners()
