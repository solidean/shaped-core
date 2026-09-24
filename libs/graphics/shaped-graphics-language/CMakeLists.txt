cmake_minimum_required(VERSION 3.28)
project(shaped-graphics-language LANGUAGES CXX)

# SGL's whole toolchain is this one library: compiler, linter, formatter and language server share the same trees.
# A command-line compiler is a thin tool over it, not a second implementation.
#
# The syntactic half, the AST pass, the check pass and the emitters need clean-core and nothing else.
# It must stay that way: an editor integration links this to parse, and should not pull in a graphics API to do it.
add_library(shaped-graphics-language
    src/shaped-graphics-language/fwd.cc

    src/shaped-graphics-language/source/diagnostic.cc
    src/shaped-graphics-language/source/format_diagnostic.cc

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

    src/shaped-graphics-language/builtins/register.cc
    src/shaped-graphics-language/builtins/register_conversions.cc
    src/shaped-graphics-language/builtins/register_scalar_math.cc
    src/shaped-graphics-language/builtins/register_textures.cc
    src/shaped-graphics-language/builtins/register_transforms.cc
    src/shaped-graphics-language/builtins/register_types.cc
    src/shaped-graphics-language/builtins/register_vector_math.cc
    src/shaped-graphics-language/builtins/registry.cc

    src/shaped-graphics-language/check/check.cc
    src/shaped-graphics-language/check/check_case.cc
    src/shaped-graphics-language/check/check_decl.cc
    src/shaped-graphics-language/check/check_pipeline.cc
    src/shaped-graphics-language/check/check_expr.cc
    src/shaped-graphics-language/check/check_resources.cc
    src/shaped-graphics-language/check/check_stmt.cc
    src/shaped-graphics-language/check/dump.cc
    src/shaped-graphics-language/check/flat.cc
    src/shaped-graphics-language/check/flat_builder.cc
    src/shaped-graphics-language/check/structural_hash.cc
    src/shaped-graphics-language/check/flatten.cc

    src/shaped-graphics-language/legalize/core.cc
    src/shaped-graphics-language/legalize/legalize_cases.cc
    src/shaped-graphics-language/legalize/legalize_exits.cc
    src/shaped-graphics-language/legalize/legalize_exprs.cc

    src/shaped-graphics-language/interpret/interpret.cc

    src/shaped-graphics-language/emit/emit.cc
    src/shaped-graphics-language/emit/reserved_words.cc
    src/shaped-graphics-language/emit/impl/hlsl.cc
    src/shaped-graphics-language/emit/impl/msl.cc
    src/shaped-graphics-language/emit/impl/plan.cc
    src/shaped-graphics-language/emit/impl/text_writer.cc
    src/shaped-graphics-language/emit/impl/wgsl.cc

    src/shaped-graphics-language/driver/compile_to_text.cc
    src/shaped-graphics-language/driver/describe.cc
    src/shaped-graphics-language/driver/pipeline_fields.cc
    src/shaped-graphics-language/driver/impl/front_end.cc
    src/shaped-graphics-language/driver/prelude.cc
)

# The prelude is two files, and only one of them is embedded.
# prelude/builtins.sgl is GENERATED from the C++ builtin registry and committed.
# The library produces the same text in memory, so it never reads that file.
# prelude/core.sgl is hand-written SGL, and the library carries its text: a page has no filesystem and a shipped binary no source tree.
# Baked in at configure time, and CMAKE_CONFIGURE_DEPENDS is what makes an edit of the file re-run this.
#
# MSVC caps ONE string literal at 16380 bytes, and adjacent literals are joined only after that limit applies.
# So the text goes out as a run of chunks that end at a newline, which also keeps a UTF-8 sequence in one piece.
set(_sgl_prelude_source "${CMAKE_CURRENT_SOURCE_DIR}/prelude/core.sgl")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_sgl_prelude_source}")
file(READ "${_sgl_prelude_source}" _sgl_prelude_text)

set(_sgl_prelude_chunk_bytes 8000)
string(LENGTH "${_sgl_prelude_text}" _sgl_prelude_length)
set(_sgl_prelude_literals "")
set(_sgl_prelude_at 0)
while(_sgl_prelude_at LESS _sgl_prelude_length)
    math(EXPR _sgl_prelude_remaining "${_sgl_prelude_length} - ${_sgl_prelude_at}")
    set(_sgl_prelude_take ${_sgl_prelude_chunk_bytes})
    if(NOT _sgl_prelude_remaining GREATER _sgl_prelude_take)
        set(_sgl_prelude_take ${_sgl_prelude_remaining})
    else()
        string(SUBSTRING "${_sgl_prelude_text}" ${_sgl_prelude_at} ${_sgl_prelude_take} _sgl_prelude_chunk)
        string(FIND "${_sgl_prelude_chunk}" "\n" _sgl_prelude_newline REVERSE)
        if(_sgl_prelude_newline GREATER -1)
            math(EXPR _sgl_prelude_take "${_sgl_prelude_newline} + 1")
        endif()
    endif()

    string(SUBSTRING "${_sgl_prelude_text}" ${_sgl_prelude_at} ${_sgl_prelude_take} _sgl_prelude_chunk)
    string(APPEND _sgl_prelude_literals "\n    R\"sglprelude(${_sgl_prelude_chunk})sglprelude\"")
    math(EXPR _sgl_prelude_at "${_sgl_prelude_at} + ${_sgl_prelude_take}")
endwhile()
# An empty core.sgl is a valid prelude, and no literal at all is not valid C++.
if(_sgl_prelude_literals STREQUAL "")
    set(_sgl_prelude_literals " \"\"")
endif()

# Written only when it differs, so a configure that changes nothing recompiles nothing.
# Not file(CONFIGURE): SGL spells an attribute `@name`, and two of them on one line would read as a CMake variable.
set(_sgl_prelude_generated "${CMAKE_CURRENT_BINARY_DIR}/generated/prelude.generated.cc")
set(_sgl_prelude_cc
"// Generated from prelude/core.sgl by shaped-graphics-language's CMakeLists. Do not edit.
#include <shaped-graphics-language/driver/prelude.hh>

namespace
{
constexpr char const* k_prelude =${_sgl_prelude_literals};
} // namespace

cc::string_view sgl::impl::embedded_core_prelude() { return k_prelude; }
")
set(_sgl_prelude_existing "")
if(EXISTS "${_sgl_prelude_generated}")
    file(READ "${_sgl_prelude_generated}" _sgl_prelude_existing)
endif()
if(NOT _sgl_prelude_existing STREQUAL _sgl_prelude_cc)
    file(WRITE "${_sgl_prelude_generated}" "${_sgl_prelude_cc}")
endif()
target_sources(shaped-graphics-language PRIVATE "${_sgl_prelude_generated}")

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
    src/shaped-graphics-language/source/format_diagnostic.hh
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

    src/shaped-graphics-language/builtins/ids.hh
    src/shaped-graphics-language/builtins/register.hh
    src/shaped-graphics-language/builtins/registry.hh

    src/shaped-graphics-language/check/check.hh
    src/shaped-graphics-language/check/checked_module.hh
    src/shaped-graphics-language/check/dump.hh
    src/shaped-graphics-language/check/flat.hh
    src/shaped-graphics-language/check/flat_builder.hh
    src/shaped-graphics-language/check/structural_hash.hh
    src/shaped-graphics-language/check/ids.hh
    src/shaped-graphics-language/check/resources.hh
    src/shaped-graphics-language/check/symbols.hh
    src/shaped-graphics-language/check/impl/checker.hh

    src/shaped-graphics-language/legalize/core.hh
    src/shaped-graphics-language/legalize/legalize.hh
    src/shaped-graphics-language/legalize/impl/legalizer.hh
    src/shaped-graphics-language/legalize/impl/walk.hh

    src/shaped-graphics-language/interpret/interpret.hh
    src/shaped-graphics-language/interpret/scalar.hh

    src/shaped-graphics-language/emit/emit.hh
    src/shaped-graphics-language/emit/reserved_words.hh
    src/shaped-graphics-language/emit/impl/dialect.hh
    src/shaped-graphics-language/emit/impl/plan.hh

    src/shaped-graphics-language/driver/compile_to_text.hh
    src/shaped-graphics-language/driver/describe.hh
    src/shaped-graphics-language/driver/pipeline_fields.hh
    src/shaped-graphics-language/driver/impl/front_end.hh
    src/shaped-graphics-language/driver/prelude.hh
)

# Require at least C++23 for this target; the final -std is chosen by the top-level project.
target_compile_features(shaped-graphics-language PUBLIC cxx_std_23)

# Expose "src" so '#include <shaped-graphics-language/...>' works for consumers.
target_include_directories(shaped-graphics-language PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_link_libraries(shaped-graphics-language PUBLIC clean-core)

# Enable conforming preprocessor for MSVC
target_compile_options(shaped-graphics-language PUBLIC $<$<CXX_COMPILER_ID:MSVC>:/Zc:preprocessor>)

# `sgl`, the command line of the toolchain: a nexus binary whose jobs are COMMANDs, `sgl prelude` and `sgl emit` so far.
# It lives with the library because it is nothing but the library behind an argument parser.
# Run it through `uv run dev.py run sgl -- <command> …`; `dev.py check`'s `sgl-prelude` step does.
# Gated like tools/: a dependent that adds shaped-core as a subdirectory wants the library and not our binaries.
if(SC_BUILD_TOOLS)
    add_executable(sgl
        tools/sgl/main.cc
        tools/sgl/files.hh
        tools/sgl/describe_command.cc
        tools/sgl/emit_command.cc
        tools/sgl/prelude_command.cc
    )
    sc_nexus_binary(sgl KINDS tool)
    # babel-data writes `describe`'s JSON; the tool may take it, and the library must not.
    target_link_libraries(sgl PRIVATE shaped-graphics-language babel-data nexus)
endif()

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

        tests/builtins/registry-test.cc

        tests/check/check-test-support.hh
        tests/check/binding-check-test.cc
        tests/check/case-check-test.cc
        tests/check/control-flow-check-test.cc
        tests/check/cube-test.cc
        tests/check/stage-check-test.cc
        tests/check/declaration-check-test.cc
        tests/check/pipeline-check-test.cc
        tests/check/enum-check-test.cc
        tests/check/expression-check-test.cc
        tests/check/source-flat-test-support.hh

        tests/interpret/interpret-test.cc

        tests/legalize/flat-test-support.hh
        tests/legalize/random-program.hh
        tests/legalize/random-program.cc
        tests/legalize/differential-test.cc
        tests/legalize/legalize-rules-test.cc

        tests/emit/emit-test-support.hh
        tests/emit/core-emit-test.cc
        tests/emit/cube-emit-test.cc
        tests/emit/emit-test.cc
        tests/emit/buffer-emit-test.cc
        tests/emit/texture-emit-test.cc
        tests/emit/compute-emit-test.cc
        tests/emit/case-emit-test.cc
        tests/emit/enum-emit-test.cc

        tests/driver/compile-to-text-test.cc
        tests/driver/describe-test.cc
        tests/driver/source-differential-test.cc

        tests/spec/spec-examples-test.cc

        tests/samples/helpers-sample-test.cc
        tests/samples/matrices-sample-test.cc
        tests/samples/samples-test.cc
    )
    sc_nexus_binary(shaped-graphics-language-test KINDS tests)

    # The spec's `sgl` fences are test input, read from the source tree so an edited example needs no reconfigure.
    target_compile_definitions(shaped-graphics-language-test
        PRIVATE
        SGL_SPEC_DIR="${CMAKE_CURRENT_SOURCE_DIR}/docs/spec"
        SGL_SAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/samples"
        SGL_PRELUDE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/prelude")

    target_link_libraries(shaped-graphics-language-test
        PRIVATE
        shaped-graphics-language
        nexus
    )

    # Browser test runner (Emscripten only); no-op natively.
    sc_add_nexus_web_runner(shaped-graphics-language-test)
endif()
