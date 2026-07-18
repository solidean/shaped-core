cmake_minimum_required(VERSION 3.28)

# instruction-tracer: Windows x64 single-step instruction tracer. See readme.md.
# Windows-only (Win32 debug API + dbghelp); the root gates this directory on
# SC_BUILD_TOOLS AND WIN32 AND TARGET zydis.

# The HTML export embeds its front-end (report/html/app.css, app.js) into a generated header, so the
# shipped .exe needs no external files. Regenerated at build time whenever a source asset changes.
set(ITRACE_HTML_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/instruction-tracer/report/html")
set(ITRACE_HTML_ASSETS "${CMAKE_CURRENT_BINARY_DIR}/generated/html_assets.hh")
add_custom_command(
    OUTPUT "${ITRACE_HTML_ASSETS}"
    COMMAND uv run "${CMAKE_CURRENT_SOURCE_DIR}/embed-html-assets.py" "${ITRACE_HTML_ASSETS}"
    DEPENDS
        "${ITRACE_HTML_DIR}/app.css"
        "${ITRACE_HTML_DIR}/app.js"
        "${CMAKE_CURRENT_SOURCE_DIR}/embed-html-assets.py"
    COMMENT "instruction-tracer: embedding HTML export assets"
    VERBATIM
)

# Split from the exe so instruction-tracer-test can link the parsing/decode/formatting code.
add_library(instruction-tracer-core STATIC
    src/instruction-tracer/cli/options.cc
    src/instruction-tracer/cli/target_spec.cc

    src/instruction-tracer/debug/debug_session.cc
    src/instruction-tracer/debug/entry_breakpoint.cc
    src/instruction-tracer/debug/mca_runner.cc
    src/instruction-tracer/debug/module_registry.cc
    src/instruction-tracer/debug/symbol_session.cc
    src/instruction-tracer/debug/trace_enrich.cc
    src/instruction-tracer/debug/trace_session.cc

    src/instruction-tracer/decode/instruction_decoder.cc
    src/instruction-tracer/decode/memory_access.cc

    src/instruction-tracer/report/console.cc
    src/instruction-tracer/report/html_export.cc
    src/instruction-tracer/report/mca.cc
    src/instruction-tracer/report/mca_timing_formatter.cc
    src/instruction-tracer/report/memory_formatter.cc
    src/instruction-tracer/report/memory_stats.cc
    src/instruction-tracer/report/source_cache.cc
    src/instruction-tracer/report/source_view.cc
    src/instruction-tracer/report/trace_formatter.cc
    src/instruction-tracer/report/trace_stats.cc

    # Generated; the custom command above makes the target depend on it.
    "${ITRACE_HTML_ASSETS}"
)

target_compile_features(instruction-tracer-core PUBLIC cxx_std_23)
target_include_directories(instruction-tracer-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# The generated html_assets.hh lives in the build tree; only html_export.cc consumes it.
target_include_directories(instruction-tracer-core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
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
        tests/decode/memory_access-test.cc
        tests/report/console-test.cc
        tests/report/html_export-test.cc
        tests/report/json_reader-test.cc
        tests/report/mca-test.cc
        tests/report/memory-test.cc
        tests/report/source_view-test.cc
        tests/report/trace_formatter-test.cc
        tests/report/trace_stats-test.cc
    )

    target_link_libraries(instruction-tracer-test PRIVATE instruction-tracer-core nexus)
endif()
