cmake_minimum_required(VERSION 3.28)

# shaped-linter: a self-contained C++ custom linter for shaped-core. See readme.md.
# Cross-platform; the root gates this directory on SC_BUILD_TOOLS alone.
# It builds its own lexer + parser (no LLVM / libclang) and lints on clean-core (cc::) + nexus.

# Split from the exe so shaped-linter-test can link the lexer / parser / rule-engine code.
add_library(shaped-linter-core STATIC
    src/shaped-linter/cli/options.cc

    src/shaped-linter/lex/source_buffer.cc
    src/shaped-linter/lex/source_manager.cc
    src/shaped-linter/lex/token.cc
    src/shaped-linter/lex/lexer.cc

    src/shaped-linter/parse/parser.cc

    src/shaped-linter/rules/member_default_init_assignment.cc
    src/shaped-linter/rules/registry.cc
    src/shaped-linter/rules/engine.cc

    src/shaped-linter/report/reporter.cc
)

target_compile_features(shaped-linter-core PUBLIC cxx_std_23)
target_include_directories(shaped-linter-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# /Zc:preprocessor: standards-conforming preprocessor on MSVC, matching the rest of the tree.
target_compile_options(shaped-linter-core PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

# clean-core is PUBLIC: it appears in our headers (cc:: everywhere), so the exe and test inherit it.
target_link_libraries(shaped-linter-core PUBLIC clean-core)

add_executable(shaped-linter src/shaped-linter/main.cc)
target_link_libraries(shaped-linter PRIVATE shaped-linter-core)

if(SC_BUILD_TESTS)
    add_executable(shaped-linter-test
        tests/main.cc

        tests/cli/options-test.cc

        tests/lex/source_buffer-test.cc
        tests/lex/lexer-test.cc

        tests/parse/parser-test.cc

        tests/rules/member_default_init_assignment-test.cc
        tests/rules/engine-test.cc
    )

    target_link_libraries(shaped-linter-test PRIVATE shaped-linter-core nexus)
endif()
