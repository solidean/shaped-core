# Minimum versions and the rationale: docs/requirements.md
cmake_minimum_required(VERSION 3.28)
project(shaped-core LANGUAGES CXX)

# Lock in C++23 for the whole repo.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# No C++20 modules anywhere in the repo, so the per-TU scan CMP0155 turns on for C++20+ buys nothing.
# It is off repo-wide rather than per preset because that is a fact about our sources, not about a platform.
# Leaving it to the default also makes the build depend on whether a `clang-scan-deps` happens to be on the
# machine: CMake pairs whichever it finds with our compiler, and a scanner cannot read a precompiled header
# another toolchain produced — which is exactly how the Android CI runner broke, having one for its Linux
# clang jobs. Turn this back on with the first `import`, and make sure the scanner matches the compiler.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

# Build gating. Both default ON for a top-level build (the normal dev flow) and OFF when
# shaped-core is consumed via add_subdirectory, where a dependent wants the libraries but not
# our test and tool binaries. Must precede add_subdirectory(extern): the tools' externals gate
# on SC_BUILD_TOOLS.
option(SC_BUILD_TESTS "Build the per-library *-test executables" ${PROJECT_IS_TOP_LEVEL})
option(SC_BUILD_TOOLS "Build the developer tools in tools/" ${PROJECT_IS_TOP_LEVEL})

# Examples are built by every normal and CI build and executed by nobody automatically — building them is
# what keeps them compiling against the API they demonstrate. `dev.py example <match>` runs exactly one.
option(SC_BUILD_EXAMPLES "Build the *-example executables" ${PROJECT_IS_TOP_LEVEL})

# Threading mode for the whole project. OFF reaches the compiler as CC_SINGLE_THREADED, which forces
# clean-core's CC_HAS_THREADS to 0 (see src/clean-core/common/macros.hh); the *-singlethreaded presets set
# it. Forcing single-threaded on a native host is what makes that mode developable without a wasm build.
#
# ON is an assertion, not a wish: a platform that cannot honor it fails configure (Emscripten.cmake) rather
# than quietly building single-threaded. So this is never a lie, whichever way it is set.
#
# No API appears or disappears with it — the threaded types fall back to running on the calling thread (see
# cc::threaded_actor, cc::async_thread_pool). It does change struct layout (node_allocation's slab header),
# so it is a whole-build switch: never set it per-target. Must precede include(Emscripten) below.
option(SC_THREADS "Build with OS thread support; OFF forces a single-threaded build" ON)

# Back cc::default_memory_resource with mimalloc; OFF uses the global operator new instead.
#
# No API changes with it and no struct layout does either — both are cc::memory_resource implementations behind the
# same pointer, so this is purely which allocator the default resource calls.
# OFF exists because tools cannot see through mimalloc: sanitizers, Valgrind and heap profilers intercept operator new
# and not mi_malloc, so LeakSanitizer never scans mimalloc memory and reports anything owned only by a cc:: container
# as a direct leak. The sanitize-* presets therefore default it OFF.
# Independent of SANITIZE on purpose: sanitizing a mimalloc build stays possible, and is the right configuration when
# what you are chasing is in mimalloc's own behavior.
option(SC_MIMALLOC "Back the default memory resource with mimalloc; OFF uses the global operator new" ON)

# Keep a frame pointer in every frame, so cc::capture_stack can walk the chain by chasing it — a few nanoseconds a
# frame, against a microsecond for the table-driven unwind that is the only alternative.
#
# Costs roughly 0.5-2% and one register on x86-64, and next to nothing on arm64 where macOS mandates the chain anyway.
# Only clang and GCC honour it: MSVC cannot maintain a frame pointer on x64 at all (/Oy is x86-only), which is why
# Windows walks with RtlCaptureStackBackTrace instead and does not care about this switch.
option(SC_FRAME_POINTERS "Keep frame pointers, so stacks can be captured by chasing the chain" ON)
if (SC_FRAME_POINTERS)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND MSVC)
        add_compile_options(/Oy-) # clang-cl spells it the MSVC way and rejects -fno-omit-frame-pointer
    elseif (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        add_compile_options(-fno-omit-frame-pointer)
    endif()
endif()

# Target CPU is x86-64. Only the instruction-tracer tool and its Zydis decoder ask — both are x86-64 by
# construction, not by omission — so this is a plain variable rather than a cached option: it describes the
# target, it is not a choice. Must precede add_subdirectory(extern), which gates the zydis build on it.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^([Aa][Mm][Dd]64|[Xx]86_64)$")
    set(SC_ARCH_X64 ON)
else()
    set(SC_ARCH_X64 OFF)
endif()

# Repo-wide build configuration helpers live in tools/cmake/.
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake")

# Apply repo-wide build configuration before add_subdirectory so every target is
# instrumented. Each is a no-op unless its driving option/preset is active.
include(Coverage)     # SC_COVERAGE  — LLVM source-based test coverage
include(Sanitizers)   # SANITIZE     — ASan/UBSan wiring (clang only)
include(PGO)          # SC_PGO_*     — IR-based profile-guided optimization (clang only)
include(Emscripten)   # EMSCRIPTEN   — WASM feature + exception-mode knobs

# Per-target precompiled headers.
# Each library declares the tiers it owns with sc_declare_pch_tier() and applies them with sc_target_pch();
# sc_finalize_pch() at the bottom of this file resolves the two together.
# Unlike the four above this one adds no flags of its own, so its position here is convention rather than a requirement.
# CMAKE_DISABLE_PRECOMPILE_HEADERS=ON (the nopch-* presets) turns every application into a no-op.
include(PrecompiledHeaders)

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

# Whether a shader compiler is available: shaped-shader-compiler-dxc links DXC, which is Windows-only and
# present only once extern/dxc has fetched it (adding the `dxc` target). Computed once, here, because
# libraries added *before* the compiler need it too — shaped-graphics' test declares a shader package, and
# an `if(TARGET ...)` there would be false no matter the real answer. A plain variable, not a cached
# option: like SC_ARCH_X64 it describes the environment, it is not a choice.
# DXC is fetched on both platforms and ssc::dxc compiles on both, so the compiler exists wherever the target does.
# Which bytecode it can emit still differs: DXIL reflection needs the Windows SDK's d3d12shader.h, and SPIR-V
# reflection reads the emitted module through the vendored SPIRV-Reflect. The consumer picks by asking a context what
# it accepts, so neither is a build-time choice.
if(TARGET dxc)
    set(SC_HAS_DXC_COMPILER ON)
else()
    set(SC_HAS_DXC_COMPILER OFF)
endif()

# Whether a window backend is available.
# shaped-rendering's sr::window is backed by SDL3, present only once extern/sdl3 has fetched it.
# Computed here for symmetry with SC_HAS_DXC_COMPILER.
# Also because an `if(TARGET ...)` down in the library would read as a local choice, when it is a fact
# about the environment.
# A plain variable, not a cached option.
if(TARGET SDL3::SDL3)
    set(SC_HAS_SDL3 ON)
else()
    set(SC_HAS_SDL3 OFF)
endif()

# Browser test-runner helper (sc_add_nexus_web_runner). Included before the libs so both clean-core and
# nexus can build an Emscripten `*-web.html` runner; a no-op on native toolchains.
include(libs/base/nexus/cmake/NexusWebRunner.cmake)

# Example helper (sc_add_example / sc_add_single_file_examples). Included before the libs so each can add
# its own examples/ subdirectory; a no-op when SC_BUILD_EXAMPLES is off.
include(libs/base/nexus/cmake/Examples.cmake)

# Shader-package helper (sc_add_shader_package). Included before the libs so any of them — including a
# library whose *test* declares a package, like shaped-graphics — can call it.
include(libs/graphics/shaped-shader-library/cmake/ShaderPackage.cmake)

# clean-core must come first: everything depends on it.
add_subdirectory(libs/base/clean-core)
add_subdirectory(libs/base/typed-geometry)

# clean-net (cnet): sockets, HTTP and a loopback dev server, over clean-core alone.
# Above clean-core rather than inside it: a TLS stack and its system libraries are a tax every binary in the repo
# would otherwise pay, and a library that can be left out expresses "absent on this platform" with a target instead
# of a maze of stubs.
add_subdirectory(libs/base/clean-net)

# data stack: babel-serializer (babel) for the format codecs, then versioned-document (vdoc) -> versioned-document-file,
# plus blob-cache (bcache) beside them.
# Added in dependency order, babel-serializer first because both persisting libraries store through babel::sqlite.
# The model library itself depends on clean-core alone, so an in-memory document never links a database engine.
add_subdirectory(libs/data/babel-serializer)
add_subdirectory(libs/data/versioned-document)
add_subdirectory(libs/data/versioned-document-file)
add_subdirectory(libs/data/blob-cache)

# graphics stack: shaped-graphics (sg) -> shaped-rendering (sr) -> shaped-viewer (sv).
# Added in dependency order; sg pulls in its per-backend static libraries itself.
add_subdirectory(libs/graphics/shaped-graphics)
add_subdirectory(libs/graphics/shaped-rendering)
add_subdirectory(libs/graphics/shaped-viewer)

# shaped-shader-compiler-dxc (ssc): HLSL -> sg::compiled_shader via DXC. Windows-only, and only when
# DXC has been built (extern/dxc adds the `dxc` target). Run `uv run extern/dxc/build-dxc.py` (dev.py
# does this automatically) to enable it.
if(SC_HAS_DXC_COMPILER)
    add_subdirectory(libs/graphics/shaped-shader-compiler-dxc)
endif()

# shaped-shader-library (slib): shader packages + hot reload, over sg. Added after ssc so its optional
# dxc compiler adapter can see that target; the packages themselves work without any compiler.
add_subdirectory(libs/graphics/shaped-shader-library)

# nexus goes LAST among the libraries even though it lives in libs/base: nothing here links it but test binaries, so
# the harness sits on top of everything it tests. It may therefore depend on any library — today babel-data, for the
# JSON sidecars — and the add order is what makes that direction readable.
add_subdirectory(libs/base/nexus)

# Developer tools. instruction-tracer needs the Win32 debug API, dbghelp and the fetched Zydis
# decoder (extern/zydis); it gates itself on the `zydis` target existing, so a checkout that has
# not fetched it still configures.
#
# x86-64 only, and not incidentally: it single-steps a debuggee through the x86-64 CONTEXT registers
# (Rip/Rsp/EFlags) and decodes with Zydis, an x86 decoder. Windows on ARM64 has neither, so the tool is
# absent there rather than ported — see SC_ARCH_X64 above.
if(SC_BUILD_TOOLS AND WIN32 AND SC_ARCH_X64 AND TARGET zydis)
    add_subdirectory(tools/instruction-tracer)
endif()

# shaped-linter: a self-contained C++ custom linter (its own lexer + parser, no LLVM). Cross-platform,
# so it gates on SC_BUILD_TOOLS alone; clean-core and nexus are already added above.
if(SC_BUILD_TOOLS)
    add_subdirectory(tools/shaped-linter)
endif()

# Cross-library examples: the ones that demonstrate several libraries together and so belong to no single
# one. Added last, after every library it may reach, and before sc_finalize_shader_packages() below because
# a graphical example declares its own shader package.
if(SC_BUILD_EXAMPLES AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt")
    add_subdirectory(examples)
endif()

# Generate the browser test-runner pages once every *-web module has been registered (Emscripten only):
# one page per library plus the aggregate tests-web.html. See cmake/NexusWebRunner.cmake.
sc_finalize_web_runners()

# Check that every declared shader package found the shaped-shader-library target. See
# libs/graphics/shaped-shader-library/cmake/ShaderPackage.cmake.
sc_finalize_shader_packages()

# Apply the recorded sc_target_pch() requests, now that every library has declared the tiers it owns.
# Deferred so a target may name a tier from a library added after its own — clean-core-test wants nexus'.
sc_finalize_pch()
