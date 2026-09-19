cmake_minimum_required(VERSION 3.28)
project(shaped-graphics-language LANGUAGES CXX)

# SGL's whole toolchain is this one library: compiler, linter, formatter and language server share the same trees.
# A command-line compiler is a thin tool over it, not a second implementation.
#
# The syntactic half and the AST pass need clean-core and nothing else.
# It must stay that way: an editor integration links this to parse, and should not pull in a graphics API to do it.
add_library(shaped-graphics-language
    src/shaped-graphics-language/fwd.cc

    src/shaped-graphics-language/source/diagnostic.cc

    src/shaped-graphics-language/lines/line_tree.cc

    src/shaped-graphics-language/tokens/token.cc
    src/shaped-graphics-language/tokens/tokenizer.cc

    src/shaped-graphics-language/groups/grouper.cc

    src/shaped-graphics-language/forms/form_parser.cc

    src/shaped-graphics-language/syntax/parsed_file.cc

    src/shaped-graphics-language/debug/dump.cc
    src/shaped-graphics-language/debug/dump_trees.cc

    src/shaped-graphics-language/ast/build.cc
    src/shaped-graphics-language/ast/build_decl.cc
    src/shaped-graphics-language/ast/build_expr.cc
    src/shaped-graphics-language/ast/build_stmt.cc
    src/shaped-graphics-language/ast/dump.cc
)

# Public headers live co-located in src/ for better editor experience.
# FILE_SET models the public API explicitly (install/export, IDEs, etc.).
target_sources(shaped-graphics-language
    PUBLIC
    FILE_SET public_headers
    TYPE HEADERS
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/src"
    FILES
    src/shaped-graphics-language/fwd.hh

    src/shaped-graphics-language/source/diagnostic.hh
    src/shaped-graphics-language/source/source_span.hh

    src/shaped-graphics-language/lines/line.hh
    src/shaped-graphics-language/lines/line_tree.hh

    src/shaped-graphics-language/tokens/token.hh
    src/shaped-graphics-language/tokens/tokenizer.hh

    src/shaped-graphics-language/groups/group.hh
    src/shaped-graphics-language/groups/grouper.hh

    src/shaped-graphics-language/forms/form.hh
    src/shaped-graphics-language/forms/form_parser.hh

    src/shaped-graphics-language/syntax/ids.hh
    src/shaped-graphics-language/syntax/parsed_file.hh

    src/shaped-graphics-language/debug/dump.hh

    src/shaped-graphics-language/ast/build.hh
    src/shaped-graphics-language/ast/decl.hh
    src/shaped-graphics-language/ast/dump.hh
    src/shaped-graphics-language/ast/expr.hh
    src/shaped-graphics-language/ast/file_ast.hh
    src/shaped-graphics-language/ast/ids.hh
    src/shaped-graphics-language/ast/parts.hh
    src/shaped-graphics-language/ast/stmt.hh
    src/shaped-graphics-language/ast/impl/builder.hh
)

# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(shaped-graphics-language PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-graphics-language/...>' works for consumers.
target_include_directories(shaped-graphics-language PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(shaped-graphics-language PUBLIC clean-core)

# Enable conforming preprocessor for MSVC
target_compile_options(shaped-graphics-language PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

# Test executable
if(SC_BUILD_TESTS)
    add_executable(shaped-graphics-language-test
        tests/main.cc

        tests/lines/line-tree-test.cc

        tests/tokens/tokenizer-test.cc

        tests/groups/grouper-test.cc

        tests/forms/form-parser-test.cc

        tests/ast/ast-test-support.hh
        tests/ast/declaration-test.cc
        tests/ast/expression-test.cc
        tests/ast/member-test.cc
        tests/ast/statement-test.cc

        tests/spec/spec-examples-test.cc

        tests/samples/samples-test.cc
    )
    sc_nexus_binary(shaped-graphics-language-test KINDS tests)

    # The spec's `sgl` fences are test input, read from the source tree so an edited example needs no reconfigure.
    target_compile_definitions(shaped-graphics-language-test
        PRIVATE
        SGL_SPEC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs/spec"
        SGL_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/samples")

    target_link_libraries(shaped-graphics-language-test
        PRIVATE
        shaped-graphics-language
        nexus
    )

    # Browser test runner (Emscripten only); no-op natively.
    sc_add_nexus_web_runner(shaped-graphics-language-test)
endif()
