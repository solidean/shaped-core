cmake_minimum_required(VERSION 3.28)

# shaped-linter: a self-contained C++ custom linter for shaped-core. See readme.md.
# Cross-platform; the root gates this directory on SC_BUILD_TOOLS alone.
# It builds its own lexer + parser (no LLVM / libclang) and lints on clean-core (cc::) + nexus.

# Concrete rules do NOT live here — each one owns a folder under rules/<group>/<rule>/ holding its
# header, implementation, tests and corpus together, and its group's CMakeLists names those files.
# This file builds the framework the rules stand on, and creates the targets they append to.

# Split from the exe so shaped-linter-test can link the lexer / parser / rule-engine code.
add_library(shaped-linter-core STATIC
    src/shaped-linter/cli/changed_lines.cc
    src/shaped-linter/cli/options.cc

    src/shaped-linter/config/glob.cc
    src/shaped-linter/config/config_parser.cc
    src/shaped-linter/config/lint_config.cc
    src/shaped-linter/config/config_resolver.cc

    src/shaped-linter/lex/source_buffer.cc
    src/shaped-linter/lex/source_language.cc
    src/shaped-linter/lex/source_manager.cc
    src/shaped-linter/lex/token.cc
    src/shaped-linter/lex/lexer.cc
    src/shaped-linter/lex/python_lexer.cc
    src/shaped-linter/lex/markdown_scanner.cc

    src/shaped-linter/parse/parser.cc

    src/shaped-linter/prose/apply.cc
    src/shaped-linter/prose/plan.cc
    src/shaped-linter/prose/prose_view.cc

    src/shaped-linter/rules/registry.cc
    src/shaped-linter/rules/engine.cc

    src/shaped-linter/report/impl/display_width.cc
    src/shaped-linter/report/snippet.cc
    src/shaped-linter/report/renderer.cc
    src/shaped-linter/report/reporter.cc
)

target_compile_features(shaped-linter-core PUBLIC cxx_std_23)
# Two roots: src/ carries the framework as <shaped-linter/…>, and the tool directory itself carries the
# rule folders as <rules/<group>/<rule>/…>, which is what lets a rule live next to its tests and corpus.
target_include_directories(shaped-linter-core PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}")
# /Zc:preprocessor: standards-conforming preprocessor on MSVC, matching the rest of the tree.
target_compile_options(shaped-linter-core PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

# clean-core is PUBLIC: it appears in our headers (cc:: everywhere), so the exe and test inherit it.
target_link_libraries(shaped-linter-core PUBLIC clean-core)

add_executable(shaped-linter src/shaped-linter/main.cc)
target_link_libraries(shaped-linter PRIVATE shaped-linter-core)

if(SC_BUILD_TESTS)
    add_executable(shaped-linter-test
        tests/main.cc

        tests/cli/changed_lines-test.cc
        tests/cli/options-test.cc

        tests/config/glob-test.cc
        tests/config/config_parser-test.cc
        tests/config/lint_config-test.cc
        tests/config/config_resolver-test.cc

        tests/lex/source_buffer-test.cc
        tests/lex/source_language-test.cc
        tests/lex/lexer-test.cc
        tests/lex/python_lexer-test.cc
        tests/lex/markdown_scanner-test.cc

        tests/parse/parser-test.cc

        tests/prose/apply-test.cc
        tests/prose/plan-test.cc
        tests/prose/prose_view-test.cc

        tests/report/display_width-test.cc
        tests/report/snippet-test.cc
        tests/report/renderer-test.cc

        tests/rules/engine-test.cc
        tests/rules/lint_corpus.cc
        tests/rules/corpus-test.cc
    )

    # babel-serializer is a test-only dependency: the corpus files are markdown, and babel::markdown
    # reads them. A tool consuming a library is the allowed direction, and it keeps shaped-linter-core
    # itself on clean-core alone.
    target_link_libraries(shaped-linter-test PRIVATE shaped-linter-core nexus babel-serializer)

    # The corpus lives next to each rule and is read at run time, so the test binary is told where the
    # rule tree starts; it is scanned recursively for *.md.
    # Adding a corpus file therefore needs no CMake change — only a re-run.
    target_compile_definitions(shaped-linter-test
        PRIVATE SCL_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/rules")
endif()

# One directory per rule group, each naming the files of the rules it holds.
# Both targets already exist above, so a group appends to them with target_sources.
add_subdirectory(rules/cpp-style)
add_subdirectory(rules/prose)
