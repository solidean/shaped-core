# Minimum versions and the rationale: docs/requirements.md
cmake_minimum_required(VERSION 3.28)
project(shaped-core LANGUAGES CXX)

# Lock in C++23 for the whole repo.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Vendored dependencies. Each SC_USE_VENDORED_<LIB> defaults ON and builds the
# in-tree copy under extern/<lib>/; turning one OFF means the embedding project
# must already provide a target of that name.
option(SC_USE_VENDORED_MIMALLOC "Build the in-tree vendored mimalloc (OFF expects an external `mimalloc` target)" ON)

# LLVM source-based test coverage. When ON, instrument every target repo-wide so
# `dev.py coverage` can collect counters from the test binaries. Driven by the
# *-coverage presets; off otherwise. Must be set before add_subdirectory so it
# reaches all targets. The flags are accepted by clang, AppleClang, and clang-cl
# (which reports CXX_COMPILER_ID "Clang"); other compilers are rejected.
option(SC_COVERAGE "Enable LLVM source-based coverage instrumentation" OFF)

if(SC_COVERAGE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SC_COVERAGE requires a Clang/LLVM toolchain (got ${CMAKE_CXX_COMPILER_ID})")
    endif()
    add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
    add_link_options(-fprofile-instr-generate -fcoverage-mapping)
endif()

# Sanitizers. SANITIZE is a comma-separated list (e.g. "address,undefined")
# wired by the *-sanitize-* presets; empty = off. Applied repo-wide before
# add_subdirectory so every target is instrumented. Clang/AppleClang/clang-cl
# only. -fno-omit-frame-pointer keeps stack traces readable in reports.
set(SANITIZE "" CACHE STRING "Comma-separated sanitizers to enable (e.g. address,undefined)")
if(SANITIZE)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SANITIZE requires a Clang/LLVM toolchain (got ${CMAKE_CXX_COMPILER_ID})")
    endif()
    add_compile_options(-fsanitize=${SANITIZE})

    if(NOT MSVC)
        # Unix clang/AppleClang: the compiler driver also links, so it wires the
        # sanitizer runtime itself. -fno-omit-frame-pointer keeps traces readable.
        add_compile_options(-fno-omit-frame-pointer)
        add_link_options(-fsanitize=${SANITIZE})
    else()
        # clang-cl. ASan is incompatible with the debug CRT (/MDd) the Debug build
        # type selects; force the non-debug dynamic CRT — also the only one the
        # Windows LLVM toolchain ships an ASan runtime for. /Oy- keeps frame
        # pointers (clang-cl rejects -fno-omit-frame-pointer).
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
        add_compile_options(/Oy-)

        # CMake links clang-cl targets with lld-link directly, bypassing the driver
        # that would add the sanitizer runtime — so add it ourselves, mirroring
        # `clang-cl -fsanitize=address -###`. In combined address+undefined mode
        # UBSan routes through the ASan runtime, so the ASan libs suffice.
        if(NOT SANITIZE MATCHES "address")
            message(FATAL_ERROR "SANITIZE='${SANITIZE}' on clang-cl: only address[,undefined] is wired for Windows")
        endif()
        execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-resource-dir
                        OUTPUT_VARIABLE _sc_resource_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
        set(SC_ASAN_RUNTIME_DIR "${_sc_resource_dir}/lib/windows" CACHE INTERNAL "clang_rt dir")
        add_link_options(
            "-libpath:${SC_ASAN_RUNTIME_DIR}"
            "${SC_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic-x86_64.lib"
            "-wholearchive:${SC_ASAN_RUNTIME_DIR}/clang_rt.asan_dynamic_runtime_thunk-x86_64.lib"
            "-include:__asan_seh_interceptor"
            "/INFERASANLIBS:NO"
        )
    endif()
endif()

# WebAssembly / Emscripten configuration. Emscripten is treated as a platform with optional features
# (threads, WebGPU) and a selectable exception mode; the knobs below are the single source of truth that
# the wasm-emscripten-* presets set. Only the single-threaded, no-WebGPU, -fexceptions combination is
# implemented today (Tier 2); the remaining combinations are reserved (Tier 3) and fail loudly so a preset
# can never silently build something untested. See the platform-support table in the README.
if(EMSCRIPTEN)
    set(SC_WASM_EXCEPTIONS "fexceptions" CACHE STRING "WASM C++ exception mode: fexceptions | wasm-exceptions")
    option(SC_WASM_THREADS "Build the multithreaded (pthreads) WASM variant" OFF)
    option(SC_WASM_WEBGPU "Build the WebGPU (emdawnwebgpu) WASM variant" OFF)

    if(SC_WASM_THREADS)
        message(FATAL_ERROR "SC_WASM_THREADS=ON: multithreaded WASM is planned (Tier 3) but not yet supported")
    endif()
    if(SC_WASM_WEBGPU)
        message(FATAL_ERROR "SC_WASM_WEBGPU=ON: WebGPU (emdawnwebgpu) WASM is planned (Tier 3) but not yet supported")
    endif()

    # nexus drives its control flow (REQUIRE / SKIP / CHECK_ASSERTS, fuzzing) through C++ exceptions, so they
    # must be enabled. Emscripten disables them by default; -fexceptions is the broadly-compatible JS-based
    # mode. -fwasm-exceptions (native wasm EH, faster, needs a newer runtime) is reserved for later.
    if(SC_WASM_EXCEPTIONS STREQUAL "fexceptions")
        add_compile_options(-fexceptions)
        add_link_options(-fexceptions)
    elseif(SC_WASM_EXCEPTIONS STREQUAL "wasm-exceptions")
        message(FATAL_ERROR "SC_WASM_EXCEPTIONS=wasm-exceptions is planned (Tier 3) but not yet supported")
    else()
        message(FATAL_ERROR "SC_WASM_EXCEPTIONS='${SC_WASM_EXCEPTIONS}': expected 'fexceptions' or 'wasm-exceptions'")
    endif()

    # Make the test executables behave like native binaries under Node: NODERAWFS gives real-filesystem
    # access (so nexus' --junit-xml std::ofstream and cwd-relative paths work), EXIT_RUNTIME propagates the
    # process exit code (pass/fail), and memory growth avoids a fixed heap cap. These are link-time settings;
    # they no-op on the static libraries and apply to the linked test binaries.
    add_link_options("SHELL:-s NODERAWFS=1" "SHELL:-s EXIT_RUNTIME=1" "SHELL:-s ALLOW_MEMORY_GROWTH=1")
endif()

if(SC_USE_VENDORED_MIMALLOC)
    add_subdirectory(extern/mimalloc)
endif()

# Browser test-runner helper (sc_add_nexus_web_runner). Included before the libs so both clean-core and
# nexus can build an Emscripten `*-web.html` runner; a no-op on native toolchains.
include(libs/base/nexus/cmake/NexusWebRunner.cmake)

# clean-core must come first: nexus depends on it.
add_subdirectory(libs/base/clean-core)
add_subdirectory(libs/base/nexus)

# Generate the browser test-runner pages once every *-web module has been registered (Emscripten only):
# one page per library plus the aggregate tests-web.html. See cmake/NexusWebRunner.cmake.
sc_finalize_web_runners()
