cmake_minimum_required(VERSION 3.28)

# instruction-tracer: Windows x64 single-step instruction tracer. See readme.md.
# Windows-only (Win32 debug API + dbghelp); the root gates this directory on
# SC_BUILD_TOOLS AND WIN32 AND TARGET zydis.

# Split from the exe so instruction-tracer-test can link the parsing/decode/formatting code.
add_library(instruction-tracer-core STATIC
    src/instruction-tracer/cli/options.cc
    src/instruction-tracer/cli/target_spec.cc

    src/instruction-tracer/debug/debug_session.cc
    src/instruction-tracer/debug/entry_breakpoint.cc
    src/instruction-tracer/debug/module_registry.cc
    src/instruction-tracer/debug/symbol_session.cc
    src/instruction-tracer/debug/trace_enrich.cc
    src/instruction-tracer/debug/trace_session.cc

    src/instruction-tracer/decode/instruction_decoder.cc

    src/instruction-tracer/report/console.cc
    src/instruction-tracer/report/source_cache.cc
    src/instruction-tracer/report/trace_formatter.cc
    src/instruction-tracer/report/trace_stats.cc
)

target_compile_features(instruction-tracer-core PUBLIC cxx_std_23)
target_include_directories(instruction-tracer-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(instruction-tracer-core PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

# dbghelp and zydis stay PRIVATE: neither appears in our headers (symbol_session hides dbghelp behind
# opaque void* handles, and only instruction_decoder.cc includes Zydis.h), so dependents need neither.
# dbghelp via target_link_libraries rather than the #pragma comment(lib) used in clean-core's native.cc.
target_link_libraries(instruction-tracer-core PUBLIC clean-core PRIVATE dbghelp zydis)

add_executable(instruction-tracer src/instruction-tracer/main.cc)
target_link_libraries(instruction-tracer PRIVATE instruction-tracer-core)

# Trace target for self-test.py: its functions are built to survive the optimizer. Not a *-test
# binary — it is a debuggee, not a nexus suite.
add_executable(instruction-tracer-fixture fixture/main.cc)
target_link_libraries(instruction-tracer-fixture PRIVATE clean-core)

if(SC_BUILD_TESTS)
    add_executable(instruction-tracer-test
        tests/main.cc

        tests/cli/options-test.cc
        tests/cli/target_spec-test.cc
        tests/debug/module_registry-test.cc
        tests/decode/instruction_decoder-test.cc
        tests/report/console-test.cc
        tests/report/trace_formatter-test.cc
        tests/report/trace_stats-test.cc
    )

    target_link_libraries(instruction-tracer-test PRIVATE instruction-tracer-core nexus)
endif()
