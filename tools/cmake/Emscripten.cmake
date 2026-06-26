# WebAssembly / Emscripten configuration (repo-wide). Included once from the root
# CMakeLists before add_subdirectory; a no-op on native toolchains.
#
# Emscripten is treated as a platform with optional features (threads, WebGPU) and
# a selectable exception mode; the knobs below are the single source of truth that
# the wasm-emscripten-* presets set. Only the single-threaded, no-WebGPU,
# -fexceptions combination is implemented today (Tier 2); the remaining combinations
# are reserved (Tier 3) and fail loudly so a preset can never silently build
# something untested. See the platform-support table in the README.

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
