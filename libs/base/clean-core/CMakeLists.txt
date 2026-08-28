cmake_minimum_required(VERSION 3.28)
project(clean-core LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# dependencies
# - mimalloc (private): backs the default general memory resource; see
#   src/clean-core/memory/mimalloc_resource.cc. Header stays private to that .cc.
# - xxhash (private): backs cc::hash128 (XXH3 128-bit); see
#   src/clean-core/bytes/hash128.cc. Header stays private to that .cc.
# - blake3 (private): backs cc::blake3 / cc::hash256 (the cryptographic hash); see
#   src/clean-core/bytes/blake3.cc. Header stays private to that .cc.
# - zstd, lz4, zlib (private): the three algorithms behind cc::compress / cc::decompress; see
#   src/clean-core/bytes/impl/. Each header stays private to its own backend .cc.

# Define the library and its sources
add_library(clean-core
    src/clean-core/common/assert.cc
    src/clean-core/common/hash.cc
    src/clean-core/common/time.cc

    src/clean-core/bytes/hash128.cc
    src/clean-core/bytes/hash256.cc
    src/clean-core/bytes/blake3.cc
    src/clean-core/bytes/compression.cc
    src/clean-core/bytes/compression_stream.cc
    src/clean-core/bytes/impl/zstd_backend.cc
    src/clean-core/bytes/impl/lz4_backend.cc
    src/clean-core/bytes/impl/deflate_backend.cc

    src/clean-core/platform/native.cc
    src/clean-core/platform/console.cc
    src/clean-core/platform/environment.cc
    src/clean-core/platform/file_path.cc
    src/clean-core/platform/stack_capture.cc
    src/clean-core/platform/module_table.cc
    src/clean-core/platform/symbolize.cc
    src/clean-core/platform/impl/text_file.cc
    src/clean-core/platform/system_info.cc
    src/clean-core/platform/system_identifier.cc
    src/clean-core/platform/resource_limits.cc
    src/clean-core/platform/system_metrics.cc
    src/clean-core/platform/process_metrics.cc
    src/clean-core/platform/storage_devices.cc
    src/clean-core/platform/network_devices.cc
    src/clean-core/platform/resource_snapshot.cc
    src/clean-core/memory/allocation.cc
    src/clean-core/memory/mimalloc_resource.cc
    src/clean-core/memory/node_allocation.cc
    src/clean-core/error/result.cc
    src/clean-core/record/domain.cc
    src/clean-core/record/chunk.cc
    src/clean-core/record/chunk_pool.cc
    src/clean-core/record/writer.cc
    src/clean-core/record/event_view.cc
    src/clean-core/record/system.cc
    src/clean-core/record/recording.cc
    src/clean-core/record/pinned_value.cc
    src/clean-core/record/value.cc
    src/clean-core/record/log.cc
    src/clean-core/record/console_listener.cc
    src/clean-core/record/impl/serialized_format.cc
    src/clean-core/record/serialize.cc
    src/clean-core/record/crash_dump.cc
    src/clean-core/record/trace.cc
    src/clean-core/record/ambient.cc
    src/clean-core/record/async_scope.cc
    src/clean-core/record/sampling.cc
    src/clean-core/record/overhead.cc
    src/clean-core/record/hot_functions.cc
    src/clean-core/record/splicing_listener.cc
    src/clean-core/error/crash_handler.cc
    src/clean-core/string/to_string.cc
    src/clean-core/string/from_string.cc
    src/clean-core/string/format.cc
    src/clean-core/string/print.cc
    src/clean-core/string/string.cc
    src/clean-core/string/string_view.cc
    src/clean-core/string/conversion.cc
    src/clean-core/string/glob.cc
    src/clean-core/string/interned_string.cc
    src/clean-core/container/vector.cc
    src/clean-core/container/byte_stream_builder.cc
    src/clean-core/container/key_value_cache.cc
    src/clean-core/thread/thread.cc
    src/clean-core/thread/threaded_actor.cc
    src/clean-core/thread/thread_pump.cc
    src/clean-core/thread/async.cc
    src/clean-core/thread/async_ambient.cc
    src/clean-core/thread/async_thread_pool.cc
    src/clean-core/streams/span_stream.cc
    src/clean-core/streams/growing_stream.cc
    src/clean-core/streams/file_stream.cc
    src/clean-core/streams/impl/native_file.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(clean-core
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/clean-core/fwd.hh

    src/clean-core/common/macros.hh
    src/clean-core/common/utility.hh
    src/clean-core/common/traits.hh
    src/clean-core/common/range_traits.hh
    src/clean-core/common/compare.hh
    src/clean-core/common/enum_traits.hh
    src/clean-core/common/flags.hh
    src/clean-core/common/hash.hh
    src/clean-core/common/endian.hh
    src/clean-core/common/assert.hh
    src/clean-core/common/time.hh
    src/clean-core/common/log.hh
    src/clean-core/common/profiling.hh
    src/clean-core/common/impl/small_size_type.hh
    src/clean-core/common/assert-handler.hh
    src/clean-core/common/assertf.hh
    src/clean-core/common/asserts.hh

    src/clean-core/bytes/hash128.hh
    src/clean-core/bytes/hash256.hh
    src/clean-core/bytes/blake3.hh
    src/clean-core/bytes/compression.hh
    src/clean-core/bytes/compression_dictionary.hh
    src/clean-core/bytes/compression_stream.hh
    src/clean-core/bytes/impl/compression_backend.hh

    src/clean-core/platform/native.hh
    src/clean-core/platform/console.hh
    src/clean-core/platform/environment.hh
    src/clean-core/platform/file_path.hh
    src/clean-core/platform/source_location.hh
    src/clean-core/platform/stacktrace.hh
    src/clean-core/platform/stack_capture.hh
    src/clean-core/platform/module_table.hh
    src/clean-core/platform/symbolize.hh
    src/clean-core/platform/impl/text_file.hh
    src/clean-core/platform/system_info.hh
    src/clean-core/platform/system_identifier.hh
    src/clean-core/platform/resource_limits.hh
    src/clean-core/platform/system_metrics.hh
    src/clean-core/platform/process_metrics.hh
    src/clean-core/platform/storage_devices.hh
    src/clean-core/platform/network_devices.hh
    src/clean-core/platform/resource_snapshot.hh
    src/clean-core/platform/win32_sanitized.hh
    src/clean-core/platform/intrinsics.hh

    src/clean-core/math/bit.hh
    src/clean-core/math/random.hh
    src/clean-core/math/wide_arith.hh

    src/clean-core/memory/allocation.hh
    src/clean-core/memory/node_allocation.hh
    src/clean-core/memory/unique_ptr.hh
    src/clean-core/memory/impl/object_lifetime_util.hh

    src/clean-core/container/array.hh
    src/clean-core/container/fixed_array.hh
    src/clean-core/container/unique_array.hh
    src/clean-core/container/vector.hh
    src/clean-core/container/fixed_vector.hh
    src/clean-core/container/unique_vector.hh
    src/clean-core/container/map.hh
    src/clean-core/container/set.hh
    src/clean-core/container/key_value_cache.hh
    src/clean-core/container/byte_stream_builder.hh
    src/clean-core/container/ringbuffer.hh
    src/clean-core/container/fixed_ringbuffer.hh
    src/clean-core/container/bitset.hh
    src/clean-core/container/fixed_bitset.hh
    src/clean-core/container/disjoint_set.hh
    src/clean-core/container/pair.hh
    src/clean-core/container/tuple.hh
    src/clean-core/container/variant.hh
    src/clean-core/container/span.hh
    src/clean-core/container/strided_span.hh
    src/clean-core/container/pinned_data.hh
    src/clean-core/container/impl/allocating_container.hh
    src/clean-core/container/impl/ringbuffer_container.hh
    src/clean-core/container/impl/bitset_container.hh

    src/clean-core/sequence/sequence.hh

    src/clean-core/algorithm/index_swap_range.hh
    src/clean-core/algorithm/permutation.hh
    src/clean-core/algorithm/search.hh
    src/clean-core/algorithm/sort.hh
    src/clean-core/algorithm/sort_async.hh
    src/clean-core/algorithm/impl/sort_impl.hh

    src/clean-core/string/string.hh
    src/clean-core/string/string_view.hh
    src/clean-core/string/char_predicates.hh
    src/clean-core/string/to_string.hh
    src/clean-core/string/from_string.hh
    src/clean-core/string/to_debug_string.hh
    src/clean-core/string/impl/format_spec.hh
    src/clean-core/string/impl/format_backend.hh
    src/clean-core/string/formatter.hh
    src/clean-core/string/format.hh
    src/clean-core/string/print.hh
    src/clean-core/string/conversion.hh
    src/clean-core/string/glob.hh
    src/clean-core/string/interned_string.hh

    src/clean-core/function/function_ref.hh
    src/clean-core/function/unique_function.hh

    src/clean-core/error/result.hh
    src/clean-core/error/optional.hh
    src/clean-core/error/crash_handler.hh

    src/clean-core/record/domain_fwd.hh
    src/clean-core/record/fwd.hh
    src/clean-core/record/desc.hh
    src/clean-core/record/domain.hh
    src/clean-core/record/chunk.hh
    src/clean-core/record/chunk_pool.hh
    src/clean-core/record/impl/thread_state.hh
    src/clean-core/record/impl/writer_tls.hh
    src/clean-core/record/impl/system_state.hh
    src/clean-core/record/writer.hh
    src/clean-core/record/event_view.hh
    src/clean-core/record/listener.hh
    src/clean-core/record/system.hh
    src/clean-core/record/record.hh
    src/clean-core/record/recording.hh
    src/clean-core/record/pinned_value.hh
    src/clean-core/record/value.hh
    src/clean-core/record/stat.hh
    src/clean-core/record/log.hh
    src/clean-core/record/scope.hh
    src/clean-core/record/console_listener.hh
    src/clean-core/record/impl/serialized_format.hh
    src/clean-core/record/serialize.hh
    src/clean-core/record/hot_functions.hh
    src/clean-core/record/splicing_listener.hh
    src/clean-core/record/crash_dump.hh
    src/clean-core/record/trace.hh
    src/clean-core/record/impl/ambient_hook.hh
    src/clean-core/record/async_scope.hh
    src/clean-core/record/sampling.hh
    src/clean-core/record/overhead.hh

    src/clean-core/thread/atomic.hh
    src/clean-core/thread/mutex.hh
    src/clean-core/thread/thread.hh
    src/clean-core/thread/thread_pump.hh
    src/clean-core/thread/threaded_actor.hh
    src/clean-core/thread/async_node.hh
    src/clean-core/thread/async.hh
    src/clean-core/thread/async_ambient.hh
    src/clean-core/thread/async_thread_pool.hh

    src/clean-core/streams/stream_flush.hh
    src/clean-core/streams/stream.hh
    src/clean-core/streams/span_stream.hh
    src/clean-core/streams/growing_stream.hh
    src/clean-core/streams/file_stream.hh
    src/clean-core/streams/impl/stream.hh
    src/clean-core/streams/impl/native_file.hh
)

# Libraries should not set a global C++ standard here.
# Instead, declare a minimum on the target so parents stay in control.
# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(clean-core
    PUBLIC
    cxx_std_23
)

# Expose "src" so '#include <clean-core/dummy.hh>' works for consumers.
target_include_directories(clean-core
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# mimalloc backs the default general memory resource; xxhash backs cc::hash128; blake3 backs cc::blake3.
# PRIVATE: only mimalloc_resource.cc / hash128.cc / blake3.cc include the respective headers;
# CMake still propagates the static libs to consumers' final link.
target_link_libraries(clean-core PRIVATE mimalloc xxhash blake3 zstd lz4 zlib)

# std::thread / pthread_setname_np (cc::threaded_actor, cc::set_current_thread_name) need the
# platform threads library on POSIX; a no-op on Windows. PUBLIC so consumers link it transitively.
find_package(Threads REQUIRED)
target_link_libraries(clean-core PUBLIC Threads::Threads)

# timeBeginPeriod / timeEndPeriod (cc::scoped_scheduler_tick) live in winmm, which is not part of the default
# Windows link set. Private: nothing in our headers names it.
if(WIN32)
    target_link_libraries(clean-core PRIVATE winmm)
endif()

# RegGetValueA (the OS build number cc::system_info reports) lives in advapi32, which is likewise not default.
# GetAdaptersAddresses (the hardware addresses cc::system_identifier reports) lives in iphlpapi.
if(WIN32)
    target_link_libraries(clean-core PRIVATE advapi32 iphlpapi psapi)
endif()

# Enable conforming preprocessor for MSVC
target_compile_options(clean-core
    PUBLIC
    $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>
)

# SC_THREADS is the root CMakeLists' option. Default it for a standalone add_subdirectory() consumer that
# has no root of ours: undefined, the generator expression below reads as OFF and would silently force a
# single-threaded build. option() would also do, but this says "fallback default", not "second declaration".
if(NOT DEFINED SC_THREADS)
    set(SC_THREADS ON)
endif()

# Define build configuration macros. These are the inputs macros.hh derives from; it owns the outputs
# (CC_ASSERT_ENABLED, CC_HAS_THREADS). PUBLIC because they must agree across the whole build: consumers
# branch on them, and CC_SINGLE_THREADED additionally changes struct layout (node_allocation's slab header),
# so a target compiled without it could not share objects with one compiled with it.
target_compile_definitions(clean-core
    PUBLIC
    $<$<CONFIG:Debug>:CC_DEBUG>
    $<$<CONFIG:Release>:CC_RELEASE>
    $<$<CONFIG:RelWithDebInfo>:CC_RELWITHDEBINFO>
    $<$<NOT:$<BOOL:${SC_THREADS}>>:CC_SINGLE_THREADED>
    $<$<BOOL:${SC_FRAME_POINTERS}>:CC_FRAME_POINTERS>
)

# Detect and link the correct library for std::stacktrace support.
# When no working <stacktrace> is found (e.g. Emscripten / WASI libc++ ships no such header), this is not
# fatal: clean-core/platform/stacktrace.hh detects the same condition via __has_include and falls back to
# an empty stub (CC_HAS_STACKTRACE 0). Only the GCC paths need an explicit link library.
include(DetectStacktraceLib)
cc_detect_stacktrace_link(_CC_STACKTRACE_LIB)

if(_CC_STACKTRACE_LIB STREQUAL "NOTFOUND")
    message(STATUS "[clean-core] std::stacktrace unavailable — building with the empty stacktrace stub")
elseif(NOT _CC_STACKTRACE_LIB STREQUAL "")
    target_link_libraries(clean-core PRIVATE ${_CC_STACKTRACE_LIB})
endif()

# DbgHelp backs the crash handler's per-thread stack walk (crash_handler.cc). std::stacktrace only ever
# captures the calling thread, so reporting the OTHER threads — the ones that matter in a deadlock — needs
# StackWalk64. Windows-only and PRIVATE: nothing in clean-core's API exposes it.
if(WIN32)
    target_link_libraries(clean-core PRIVATE dbghelp)
endif()

# Detect and use the fastest available linker (mold > lld > system default)
include(DetectLinker)
cc_detect_linker(_CC_LINKER)

if(NOT _CC_LINKER STREQUAL "" AND NOT _CC_LINKER STREQUAL "msvc")
    target_link_options(clean-core PUBLIC -fuse-ld=${_CC_LINKER})
endif()

# The PCH tiers clean-core owns. Every other library's tiers build on these, so they are declared first in the repo.
# docs/guides/precompiled-headers.md is how to change one; the short version is that it is a measurement, not a guess.

# The clean-core surface a typical TU reaches for.
sc_declare_pch_tier(CC HEADERS
    <clean-core/fwd.hh>
    <clean-core/common/assert.hh>
    <clean-core/common/macros.hh>
    <clean-core/common/utility.hh>
    <clean-core/common/hash.hh>
    <clean-core/container/span.hh>
    <clean-core/container/vector.hh>
    <clean-core/container/array.hh>
    <clean-core/container/map.hh>
    <clean-core/container/set.hh>
    <clean-core/container/pair.hh>
    <clean-core/container/tuple.hh>
    <clean-core/container/variant.hh>
    <clean-core/container/small_vector.hh>
    <clean-core/container/fixed_vector.hh>
    <clean-core/string/string.hh>
    <clean-core/string/string_view.hh>
    <clean-core/string/format.hh>
    <clean-core/error/optional.hh>
    <clean-core/error/result.hh>
    <clean-core/function/function_ref.hh>
    <clean-core/function/unique_function.hh>
    <clean-core/memory/unique_ptr.hh>
    <clean-core/memory/shared_ptr.hh>
    <clean-core/memory/allocation.hh>
)

# The heavy tail: threading, streams, node allocation.
# A rung of its own because these churn most, and a given TU is least likely to need them.
sc_declare_pch_tier(CC_ALL EXTENDS CC HEADERS
    <clean-core/thread/mutex.hh>
    <clean-core/thread/atomic.hh>
    <clean-core/thread/async.hh>
    <clean-core/thread/thread.hh>
    <clean-core/memory/node_allocation.hh>
    <clean-core/streams/stream.hh>
    <clean-core/streams/file_stream.hh>
    <clean-core/container/pinned_data.hh>
    <clean-core/container/ringbuffer.hh>
    <clean-core/math/random.hh>
    <clean-core/platform/console.hh>
    <clean-core/platform/stacktrace.hh>
    <clean-core/sequence/sequence.hh>
    <clean-core/common/log.hh>
    <clean-core/common/profiling.hh>
    <clean-core/record/record.hh>
)

# CC_ALL plus the expensive standard headers our containers and strings reach anyway.
# A target that uses none of them is measurably worse off here than at CC_ALL — see shaped-linter-core.
sc_declare_pch_tier(CC_STD EXTENDS CC_ALL STD)

sc_target_pch(clean-core CC_STD) # 0.47

# Test executable
if(SC_BUILD_TESTS)
    add_executable(clean-core-test
        tests/main.cc

        tests/benchmarks/string-hash-bench.cc
        tests/benchmarks/hash-benchmark.cc
        tests/benchmarks/allocation-benchmark.cc
        tests/benchmarks/async/async-benchmark.cc
        tests/benchmarks/async/born-ready-benchmark.cc
        tests/benchmarks/async/pool-benchmark.cc
        tests/benchmarks/async/grain-benchmark.cc
        tests/benchmarks/node-allocation-design-benchmark.cc
        tests/benchmarks/node-allocation-design-refill.cc
        tests/benchmarks/file-stream-benchmark.cc
        tests/benchmarks/sort-benchmark.cc
        tests/benchmarks/compression-benchmark.cc

        # tests grouped by topic, mirroring src/clean-core/
        tests/common/assert-test.cc
        tests/common/compare-test.cc
        tests/common/flags-test.cc
        tests/common/hash-test.cc
        tests/common/hash-types-test.cc
        tests/common/endian-test.cc
        tests/common/invocable-test.cc
        tests/common/macros-test.cc
        tests/common/utility-test.cc
        tests/common/time-test.cc

        tests/bytes/hash128-test.cc
        tests/bytes/hash256-test.cc
        tests/bytes/blake3-test.cc
        tests/bytes/compression-test.cc
        tests/bytes/compression-stream-test.cc

        tests/platform/console-test.cc
        tests/platform/environment-test.cc
        tests/platform/file_path-test.cc
        tests/platform/stack-capture-test.cc
        tests/platform/symbolize-test.cc
        tests/platform/system_info-test.cc
        tests/platform/system_identifier-test.cc
        tests/platform/resource_limits-test.cc
        tests/platform/system_metrics-test.cc
        tests/platform/process_metrics-test.cc
        tests/platform/storage_devices-test.cc
        tests/platform/network_devices-test.cc
        tests/platform/resource_snapshot-test.cc

        tests/algorithm/sort-test.cc
        tests/algorithm/select-test.cc
        tests/algorithm/search-test.cc
        tests/algorithm/permutation-test.cc
        tests/algorithm/sort-async-test.cc

        tests/math/bit-test.cc
        tests/math/random-test.cc
        tests/math/wide_arith-test.cc

        tests/memory/allocation-test.cc
        tests/memory/node_allocation-test.cc
        tests/memory/node_allocation-default-test.cc
        tests/memory/node_allocation-patterns-test.cc
        tests/memory/node_allocation-sizes-test.cc
        tests/memory/node_allocation-slabs-test.cc
        tests/memory/node_allocation-handles-test.cc
        tests/memory/unique_ptr-test.cc
        tests/memory/shared_ptr-test.cc

        tests/container/array-test.cc
        tests/container/tuple-test.cc
        tests/container/variant-test.cc
        tests/container/fixed-array-test.cc
        tests/container/small_vector-test.cc
        tests/container/fixed_vector-test.cc
        tests/container/span-test.cc
        tests/container/strided_span-test.cc
        tests/container/pinned_data-test.cc
        tests/container/vector-test.cc
        tests/container/unique_vector-test.cc
        tests/container/ringbuffer-test.cc
        tests/container/fixed_ringbuffer-test.cc
        tests/container/bitset-test.cc
        tests/container/fixed_bitset-test.cc
        tests/container/disjoint_set-test.cc
        tests/container/map-test.cc
        tests/container/set-test.cc
        tests/container/byte_stream_builder-test.cc
        tests/container/key_value_cache-test.cc

        tests/string/conversion-test.cc
        tests/string/format-test.cc
        tests/string/from_string-test.cc
        tests/string/glob-test.cc
        tests/string/interned_string-test.cc
        tests/string/print-test.cc
        tests/string/string-test.cc
        tests/string/string_view-test.cc
        tests/string/to_chars-test.cc
        tests/string/to_debug_string-test.cc

        tests/function/function_ref-test.cc
        tests/function/unique_function-test.cc

        tests/error/optional-test.cc
        tests/error/result-test.cc

        tests/streams/stream-test.cc
        tests/streams/span_stream-test.cc
        tests/streams/growing_stream-test.cc
        tests/streams/file_stream-test.cc
        tests/streams/read_line-test.cc

        tests/record/record-test.cc
        tests/record/record-domain-test.cc
        tests/record/record-console-test.cc
        tests/record/record-adoption-test.cc
        tests/record/record-frontends-test.cc
        tests/record/record-query-test.cc
        tests/record/record-serialize-test.cc
        tests/record/record-trace-test.cc
        tests/record/record-async-scope-test.cc
        tests/record/record-sampling-test.cc
    tests/record/record-hot-functions-test.cc
    tests/record/record-retention-test.cc
    tests/record/record-splicing-test.cc

        tests/thread/async-test.cc
        tests/thread/async-ambient-test.cc
        tests/thread/async-coroutine-test.cc
        tests/thread/async-pool-test.cc
        tests/thread/atomic-test.cc
        tests/thread/chase_lev_deque-test.cc
        tests/thread/mutex-test.cc
        tests/thread/thread-test.cc
        tests/thread/thread_pump-test.cc
        tests/thread/threaded_actor-test.cc
    )

    target_link_libraries(clean-core-test
        PRIVATE
        clean-core
        nexus
        xxhash # tests/benchmarks/hash-benchmark.cc calls the raw XXH3 entry points directly
        blake3 # and the raw BLAKE3 ones, in the same table
    )

    sc_target_pch(clean-core-test CC_STD NEXUS) # 0.67

    # Browser test runner (Emscripten only): clean-core-test-web.html drives these tests one per frame.
    sc_add_nexus_web_runner(clean-core-test)
endif()

# Examples: how the API reads in practice, one binary per file. See docs/guides/examples.md.
add_subdirectory(examples)

# Coverage
option(CC_COVERAGE_MSVC "Enable MSVC native coverage" OFF)

# TARGET clean-core-test: coverage instruments the test binary, which SC_BUILD_TESTS=OFF does not build.
if(CC_COVERAGE_MSVC AND MSVC AND TARGET clean-core-test)
    # Debug info is required for sensible mapping
    target_compile_options(clean-core PRIVATE /Zi /Od)
    target_compile_options(clean-core-test PRIVATE /Zi /Od)

    # THIS is the critical bit to enable coverage
    target_link_options(clean-core-test PRIVATE /PROFILE)

    # Optional but helps avoid weirdness
    target_link_options(clean-core-test PRIVATE /DEBUG:FULL /INCREMENTAL:NO)
endif()

# Configuration diagnostics
option(CC_PRINT_CONFIG "Print clean-core configuration diagnostics" ON)

if(CC_PRINT_CONFIG)
    # Linker display name
    if(_CC_LINKER STREQUAL "")
        set(_cc_linker_name "system default")
    else()
        set(_cc_linker_name "${_CC_LINKER}")
    endif()

    # Threading mode: worth printing because SC_THREADS=OFF changes both behavior and struct layout, and a
    # stale build directory is otherwise indistinguishable from a fresh one.
    if(SC_THREADS)
        set(_cc_threads_name "OS threads (SC_THREADS=ON)")
    else()
        set(_cc_threads_name "single-threaded (SC_THREADS=OFF -> CC_HAS_THREADS 0)")
    endif()

    message(STATUS "")
    message(STATUS "[clean-core] Configuration:")
    if(CMAKE_C_COMPILER_ID)
        message(STATUS "  C compiler   : ${CMAKE_C_COMPILER} (${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION})")
    endif()
    message(STATUS "  C++ compiler : ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION})")
    message(STATUS "  OS           : ${CMAKE_SYSTEM_NAME}")
    message(STATUS "  Linker       : ${_cc_linker_name}")
    message(STATUS "  Threading    : ${_cc_threads_name}")
    message(STATUS "")
endif()
