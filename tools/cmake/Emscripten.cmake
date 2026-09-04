# WebAssembly / Emscripten configuration (repo-wide). Included once from the root
# CMakeLists before add_subdirectory; a no-op on native toolchains.
#
# Emscripten is treated as a platform with two optional features -- threads and WebGPU -- and a selectable
# exception mode. The knobs here are the single source of truth that the wasm-emscripten-* presets set.
# Threads are the exception: they are the repo-wide SC_THREADS option (root CMakeLists), since forcing
# single-threaded is useful on every platform, not just this one. This file only enforces what wasm can honor.
#
# All four threads x WebGPU combinations configure. They are genuinely different deployment tiers rather than
# a build-type matrix: WebGPU needs no cross-origin isolation, so a no-threads WebGPU build is droppable on
# any static host, while anything with threads needs SharedArrayBuffer and therefore COOP/COEP headers.
# See the platform-support table in the README.

if(EMSCRIPTEN)
    set(SC_WASM_EXCEPTIONS "fexceptions" CACHE STRING "WASM C++ exception mode: fexceptions | wasm-exceptions")
    option(SC_WASM_WEBGPU "Build the WebGPU (emdawnwebgpu) WASM variant" OFF)

    # Threads are the repo-wide SC_THREADS knob (root CMakeLists), not a wasm-local one.
    # -pthread is what predefines __EMSCRIPTEN_PTHREADS__, which is what clean-core's CC_HAS_THREADS reads --
    # so passing it here is the whole of the C++-side wiring (see common/macros.hh).
    if(SC_THREADS)
        add_compile_options(-pthread)
        add_link_options(-pthread)

        # The pool must be warm before main() runs.
        # A test that spawns a thread and joins it blocks the thread that spawned, and a worker created on demand
        # is serviced by that same event loop -- which the join has stopped. That is a deadlock, not a slowdown.
        # _STRICT=0 keeps exhausting the pool a warning plus an on-demand spawn rather than a hard error.
        add_link_options("SHELL:-s PTHREAD_POOL_SIZE=8" "SHELL:-s PTHREAD_POOL_SIZE_STRICT=0")
    endif()

    # emdawnwebgpu ships inside the emsdk, so WebGPU costs no vendored dependency -- which is the reason this
    # tier is cheaper to reach than a native Dawn backend would be.
    # Needed on compile and link both: the port supplies webgpu.h to one and its JS bindings to the other.
    if(SC_WASM_WEBGPU)
        add_compile_options("SHELL:--use-port=emdawnwebgpu")
        add_link_options("SHELL:--use-port=emdawnwebgpu")
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
    # access (so nexus' --junit-xml file write and cwd-relative paths work), EXIT_RUNTIME propagates the
    # process exit code (pass/fail), and memory growth avoids a fixed heap cap. These are link-time settings;
    # they no-op on the static libraries and apply to the linked test binaries.
    add_link_options("SHELL:-s NODERAWFS=1" "SHELL:-s EXIT_RUNTIME=1" "SHELL:-s ALLOW_MEMORY_GROWTH=1")
endif()
