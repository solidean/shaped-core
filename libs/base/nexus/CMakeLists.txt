cmake_minimum_required(VERSION 3.28)
project(nexus LANGUAGES CXX)

# Define the library and its sources
add_library(nexus
    src/nexus/run.cc
    src/nexus/args/builder.cc
    src/nexus/args/diagnostic.cc
    src/nexus/args/value.cc
    src/nexus/args/validation.cc
    src/nexus/args/tokenize.cc
    src/nexus/args/ambient.cc
    src/nexus/args/impl/os_args.cc
    src/nexus/args/impl/describe.cc
    src/nexus/args/completion.cc
    src/nexus/args/impl/help_render.cc
    src/nexus/args/impl/parse_engine.cc
    src/nexus/args/impl/setup_check.cc
    src/nexus/args/impl/suggest.cc
    src/nexus/rec.cc
    src/nexus/test.cc
    src/nexus/pgo.cc
    src/nexus/bench/barriers.cc
    src/nexus/bench/calibration.cc
    src/nexus/bench/report.cc
    src/nexus/bench/run.cc
    src/nexus/bench/statistics.cc
    src/nexus/bench/hardware_counters.cc
    src/nexus/tests/alias.cc
    src/nexus/tests/check.cc
    src/nexus/tests/config.cc
    src/nexus/tests/invoke_tests.cc
    src/nexus/tests/execute.cc
    src/nexus/tests/registry.cc
    src/nexus/tests/schedule.cc
    src/nexus/tests/export/xml.cc
    src/nexus/tests/export/catch2.cc
    src/nexus/tests/export/junit.cc
    src/nexus/tests/export/pgo_json.cc
    src/nexus/tests/export/listing_json.cc
    src/nexus/fuzz/machine.cc
    src/nexus/fuzz/runner.cc
    src/nexus/fuzz/run.cc
    src/nexus/fuzz/test.cc
)
# Note: src/nexus/web/web_runner.cc is intentionally NOT a library source. It is Emscripten-only and is
# compiled directly into each `*-test-web` runner (see cmake/NexusWebRunner.cmake) so the linker can't
# prune its EMSCRIPTEN_KEEPALIVE exports as unreferenced archive members.

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(nexus
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/nexus/fwd.hh
    src/nexus/run.hh
    src/nexus/args/args.hh
    src/nexus/args/builder.hh
    src/nexus/args/diagnostic.hh
    src/nexus/args/fwd.hh
    src/nexus/args/options.hh
    src/nexus/args/parse_args.hh
    src/nexus/args/value.hh
    src/nexus/args/validation.hh
    src/nexus/args/tokenize.hh
    src/nexus/args/ambient.hh
    src/nexus/args/completion.hh
    # Not public API, but builder.hh includes both, so a consumer that gets builder.hh needs them too.
    src/nexus/args/impl/binding.hh
    src/nexus/args/impl/command.hh
    src/nexus/rec.hh
    src/nexus/impl/rec_session.hh
    src/nexus/test.hh
    src/nexus/pgo.hh
    src/nexus/bench/fwd.hh
    src/nexus/bench/barriers.hh
    src/nexus/bench/calibration.hh
    src/nexus/bench/report.hh
    src/nexus/bench/result.hh
    src/nexus/bench/run.hh
    src/nexus/bench/run_config.hh
    src/nexus/bench/statistics.hh
    src/nexus/bench/hardware_counters.hh
    src/nexus/bench/bench.hh
    src/nexus/tests/alias.hh
    src/nexus/tests/check.hh
    src/nexus/tests/config.hh
    src/nexus/tests/invoke_tests.hh
    src/nexus/tests/execute.hh
    src/nexus/tests/registry.hh
    src/nexus/tests/schedule.hh
    src/nexus/tests/thread_scope.hh
    src/nexus/tests/typed_value.hh
    src/nexus/tests/typed_invoke.hh
    src/nexus/tests/export/xml.hh
    src/nexus/tests/export/catch2.hh
    src/nexus/tests/export/junit.hh
    src/nexus/tests/export/pgo_json.hh
    src/nexus/tests/export/listing_json.hh

    src/nexus/fuzz/fwd.hh
    src/nexus/fuzz/signature.hh
    src/nexus/fuzz/operation.hh
    src/nexus/fuzz/executed_operation.hh
    src/nexus/fuzz/machine.hh
    src/nexus/fuzz/runner.hh
    src/nexus/fuzz/run.hh
    src/nexus/fuzz/regression_dialect.hh
    src/nexus/fuzz/test.hh
    src/nexus/fuzz/fuzz.hh
)

# Libraries should not set a global C++ standard here.
# Instead, declare a minimum on the target so parents stay in control.
# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(nexus
    PUBLIC
    cxx_std_23
)

# Expose "src" so '#include <nexus/dummy.hh>' works for consumers.
target_include_directories(nexus
    PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(nexus
    PUBLIC
    clean-core
    # nexus is a leaf: the harness sits on top of the libraries it tests, so babel::json writing the
    # listing and pgo sidecars is a dependency going DOWN, not an upward one.
    #
    # babel-data rather than babel-serializer, and that constraint is nexus' rather than babel's: EVERY *-test binary
    # in the repo links nexus, so whatever nexus depends on is a tax the whole tree pays and a link every test binary
    # waits for. Only libraries that pull in no external of their own belong here.
    PRIVATE
    babel-data
)

# nexus/bench hardware-counter backend: exactly one per platform.
# NX_BENCH_HAS_HW_COUNTERS (1/0) is a coarse compile-time hint "was a real PMU backend compiled in";
# the runtime source of truth is always nx::bench::available_hw_counters().
if(WIN32)
    target_sources(nexus PRIVATE src/nexus/bench/impl/hardware_counters_windows.cc)
    # advapi32: the ETW control + real-time consumer APIs (StartTrace / TraceSetInformation / OpenTrace /
    # ProcessTrace / ControlTrace) plus the token APIs used to enable SeSystemProfilePrivilege.
    target_link_libraries(nexus PUBLIC advapi32)
    target_compile_definitions(nexus PUBLIC NX_BENCH_HAS_HW_COUNTERS=1)
elseif(LINUX)
    target_sources(nexus PRIVATE src/nexus/bench/impl/hardware_counters_linux.cc)
    target_compile_definitions(nexus PUBLIC NX_BENCH_HAS_HW_COUNTERS=1)
else()
    target_sources(nexus PRIVATE src/nexus/bench/impl/hardware_counters_none.cc)
    target_compile_definitions(nexus PUBLIC NX_BENCH_HAS_HW_COUNTERS=0)
endif()

# EXTENDS CC rather than a higher rung: nexus/test.hh reaches only CC-tier clean-core headers plus <typeindex>,
# so a pure clean-core test binary can take `CC NEXUS` without paying for the STL block.
# Naming CC as the parent is also what keeps clean-core at the front of the list when NEXUS is applied alongside a taller tier.
sc_declare_pch_tier(NEXUS EXTENDS CC HEADERS <nexus/test.hh>)

sc_target_pch(nexus CC)

# Test executable
if(SC_BUILD_TESTS)
    add_executable(nexus-test
        tests/main.cc
        tests/args-grammar-test.cc
        tests/args-value-test.cc
        tests/args-diagnostic-test.cc
        tests/args-setup-test.cc
        tests/args-help-test.cc
        tests/args-validation-test.cc
        tests/args-commands-test.cc
        tests/args-struct-test.cc
        tests/args-tokenize-test.cc
        tests/args-ambient-test.cc
        tests/args-completion-test.cc
        tests/test-api-test.cc
        tests/test-cli-test.cc
        tests/test-test-args-test.cc
        tests/test-check-test.cc
        tests/test-recording-test.cc
        tests/test-check-render-test.cc
        tests/test-threaded-check-test.cc
        tests/test-parallel-test.cc
        tests/test-async-test.cc
        tests/test-export-test.cc
        tests/test-registry-test.cc
        tests/test-section-test.cc
        tests/test-invocable-test.cc
        tests/test-alias-test.cc
        tests/test-typed-value-test.cc
        tests/bench-barriers-test.cc
        tests/bench-statistics-test.cc
        tests/bench-run-test.cc
        tests/bench-report-test.cc
        tests/bench-hardware-counters-test.cc
        tests/bench-cache-traversal-test.cc
        tests/fuzz-engine-test.cc
        tests/fuzz-minimize-test.cc
        tests/fuzz-regression-test.cc
    )

    target_link_libraries(nexus-test
        PRIVATE
        nexus
        babel-data # the export tests read the listing JSON back rather than matching its text
    )

    # CC rather than a bigger tier, and measured: nexus-test goes 0.63 at CC but 0.84 once the heavy STL block is added.
    # It is the counterexample that keeps NEXUS an explicit keyword rather than something inferred from a `-test` suffix.
    sc_target_pch(nexus-test CC)

    # Browser test runner (Emscripten only): nexus-test-web.html drives these tests one per frame.
    sc_add_nexus_web_runner(nexus-test)
endif()

# The examples corpus: nexus has owned the mechanism since sc_add_example landed, and now uses it too.
# A no-op when SC_BUILD_EXAMPLES is off, which sc_add_single_file_examples handles for us.
add_subdirectory(examples)
