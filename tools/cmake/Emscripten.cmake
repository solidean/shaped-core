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

    # A native stack, where emscripten's default is 64 KiB.
    # A tree walk here bounds its depth rather than its frames, and a native thread's megabyte is what that bound was
    # sized against: sgl's interpreter overflows 64 KiB at the nesting it accepts.
    # The pthread default is the same 64 KiB, so a worker gets the megabyte too.
    sc_build_options(LINK "SHELL:-s STACK_SIZE=1MB")

    # Threads are the repo-wide SC_THREADS knob (root CMakeLists), not a wasm-local one.
    # -pthread is what predefines __EMSCRIPTEN_PTHREADS__, which is what clean-core's CC_HAS_THREADS reads --
    # so passing it here is the whole of the C++-side wiring (see common/macros.hh).
    if(SC_THREADS)
        sc_build_options(COMPILE -pthread)
        sc_build_options(LINK -pthread)

        # The pool must be warm before main() runs.
        # A test that spawns a thread and joins it blocks the thread that spawned, and a worker created on demand
        # is serviced by that same event loop -- which the join has stopped. That is a deadlock, not a slowdown.
        # _STRICT=0 keeps exhausting the pool a warning plus an on-demand spawn rather than a hard error.
        sc_build_options(LINK "SHELL:-s PTHREAD_POOL_SIZE=8" "SHELL:-s PTHREAD_POOL_SIZE_STRICT=0")
        sc_build_options(LINK "SHELL:-s DEFAULT_PTHREAD_STACK_SIZE=1MB")
    endif()

    # WebAssembly's 128-bit SIMD, which clean-simd's simd128 kernel is built on.
    # On by default: every current engine runs it (Chrome and Firefox since 2021, Safari since 16.4, the emsdk's Node).
    # OFF builds the scalar kernel instead, for an engine that predates it.
    option(SC_WASM_SIMD "Build with WebAssembly SIMD128 (-msimd128)" ON)
    if(SC_WASM_SIMD)
        sc_build_options(COMPILE -msimd128)
    endif()

    # emdawnwebgpu ships inside the emsdk, so WebGPU costs no vendored dependency -- which is the reason this
    # tier is cheaper to reach than a native Dawn backend would be.
    # Needed on compile and link both: the port supplies webgpu.h to one and its JS bindings to the other.
    if(SC_WASM_WEBGPU)
        sc_build_options(COMPILE "SHELL:--use-port=emdawnwebgpu")
        sc_build_options(LINK "SHELL:--use-port=emdawnwebgpu")
    endif()

    # Keep the wasm name section, so a captured stack reads as function names rather than as bare indices.
    # cc::stacktrace's Emscripten backend renders whatever emscripten_get_callstack can see, and without this it can
    # see only numbers -- which is the difference between a usable assert and a useless one.
    # Release is left stripped: those names are a large part of the size a release wasm build exists to avoid.
    #
    # It also decides CC_WASM_KEEPS_FRAME_STRUCTURE, which is a separate observation about the same builds.
    # A Release wasm build collapses calls that the source keeps apart: two functions marked CC_DONT_INLINE, neither
    # tail-calling, still arrive as one frame in the engine's stack trace, while the same source at RelWithDebInfo
    # reports both.
    # Which stage does it -- LLVM, or the Binaryen pass that runs after and never saw the C++ attribute -- is not
    # pinned down here, so this says only what is measured.
    # Nothing but a test should care: a walk reports the frames that exist, and these are the frames that exist.
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        sc_build_options(COMPILE --profiling-funcs)
        sc_build_options(LINK --profiling-funcs)
        sc_build_options(DEFINITIONS CC_WASM_KEEPS_FRAME_STRUCTURE=1)
    else()
        sc_build_options(DEFINITIONS CC_WASM_KEEPS_FRAME_STRUCTURE=0)
    endif()

    # Debug info BESIDE the artifact, for resolving a captured stack after the fact.
    #
    # A stripped build's stack is still byte offsets into the module file, and those offsets are byte-identical to
    # what a named build of the same source produces -- measured.
    # So the names can live outside the binary entirely.
    #
    # **The two sidecars are not equally priced, which is why this is not one switch.**
    # Measured on a small module at -O3, against a 13,249-byte baseline:
    #
    #   -gsource-map       13,281 bytes (+0.2%), name section still stripped -- a browser reads it with no tooling
    #   -gseparate-dwarf   15,278 bytes (+15.3%), name section KEPT -- llvm-symbolizer resolves it, names ship again
    #
    # On clean-core-test the DWARF option cost 1.9 MB of a 8.2 MB artifact.
    # So neither is implied by Release: turning them on there silently would have grown every release artifact to
    # buy a capability nobody asked for, and the DWARF one would have left it un-stripped as well -- making the
    # offline path moot for the one build that needs it.
    #
    # Nothing checks a sidecar against the stack it resolves, and nothing here can: an offset from another build of
    # the same source resolves to a plausible, confident, wrong name.
    # Keeping the sidecar beside the artifact it was built with is what stands in for a build identity.
    set(SC_WASM_DEBUG_SIDECARS "off" CACHE STRING "WASM debug sidecars: off | source-map | dwarf | both")

    # Lower-cased before it is matched, so OFF -- which is what a bool-shaped cache entry or a habit produces --
    # means what it obviously means rather than failing configure.
    string(TOLOWER "${SC_WASM_DEBUG_SIDECARS}" sc_wasm_sidecars)
    if(NOT sc_wasm_sidecars MATCHES "^(off|source-map|dwarf|both)$")
        message(FATAL_ERROR "SC_WASM_DEBUG_SIDECARS='${SC_WASM_DEBUG_SIDECARS}': expected off, source-map, dwarf or both")
    endif()

    if(sc_wasm_sidecars STREQUAL "source-map" OR sc_wasm_sidecars STREQUAL "both")
        sc_build_options(COMPILE -gsource-map)
        sc_build_options(LINK -gsource-map)
    endif()
    if(sc_wasm_sidecars STREQUAL "dwarf" OR sc_wasm_sidecars STREQUAL "both")
        sc_build_options(COMPILE -gseparate-dwarf)
        sc_build_options(LINK -gseparate-dwarf)
    endif()

    # nexus drives its control flow (REQUIRE / SKIP / CHECK_ASSERTS, fuzzing) through C++ exceptions, so they
    # must be enabled. Emscripten disables them by default.
    # fexceptions is the JS-based mode: every call that may throw from a frame with cleanups goes out through a JS
    # `invoke_*` wrapper, a round trip that blocks inlining and puts a JS frame on the engine's stack per call.
    # wasm-exceptions is wasm's own exception handling: no wrappers, the engine unwinds.
    # It emits the proposal's legacy instructions, which every engine that runs shared memory and SIMD128 runs too.
    # setjmp/longjmp must use the same mechanism, or a C library that uses them fails to link.
    # Every object of a link must be built in one mode, so this is a repo-wide knob.
    if(SC_WASM_EXCEPTIONS STREQUAL "fexceptions")
        sc_build_options(COMPILE -fexceptions)
        sc_build_options(LINK -fexceptions)
    elseif(SC_WASM_EXCEPTIONS STREQUAL "wasm-exceptions")
        sc_build_options(COMPILE -fwasm-exceptions "SHELL:-s SUPPORT_LONGJMP=wasm")
        sc_build_options(LINK -fwasm-exceptions "SHELL:-s SUPPORT_LONGJMP=wasm" "SHELL:-s WASM_LEGACY_EXCEPTIONS=1")
    else()
        message(FATAL_ERROR "SC_WASM_EXCEPTIONS='${SC_WASM_EXCEPTIONS}': expected 'fexceptions' or 'wasm-exceptions'")
    endif()

    # Make the test executables behave like native binaries under Node: NODERAWFS gives real-filesystem
    # access (so nexus' --junit-xml file write and cwd-relative paths work), EXIT_RUNTIME propagates the
    # process exit code (pass/fail), and memory growth avoids a fixed heap cap. These are link-time settings;
    # they no-op on the static libraries and apply to the linked test binaries.
    sc_build_options(LINK "SHELL:-s NODERAWFS=1" "SHELL:-s EXIT_RUNTIME=1" "SHELL:-s ALLOW_MEMORY_GROWTH=1")

    # A link runs wasm-opt, which takes about 2 GB per process, so a full test build links more binaries at once
    # than a developer machine has memory for; compiles stay at full parallelism.
    # A Ninja job pool, so another generator ignores it.
    set(SC_WASM_LINK_JOBS 4 CACHE STRING "How many wasm links (each running wasm-opt) may run at once")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS sc_wasm_link=${SC_WASM_LINK_JOBS})
    set(CMAKE_JOB_POOL_LINK sc_wasm_link)
endif()
