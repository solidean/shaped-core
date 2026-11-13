#pragma once

#include "analysis.hh"
#include "protocol/server.hh"
#include "protocol/types.hh"

#include <clean-core/container/vector.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/thread/async.hh>
#include <shaped-graphics-language/test/run_tests.hh>

/// Each feature is one translation: an analysis in, LSP structures out, in the negotiated position encoding.
/// They are pure functions, so they run on compute and a test calls them directly.
namespace sgl_lsp
{
/// The LSP range of a span of file `file`.
[[nodiscard]] lsp::range range_of(analysis const& a, i32 file, sgl::source_span where, lsp::position_encoding e);

/// The document's diagnostics: every phase's, plus a failure per test in `tests` that did not pass.
[[nodiscard]] cc::vector<lsp::diagnostic> diagnostics_of(analysis const& a,
                                                         cc::span<sgl::test::test_result const> tests,
                                                         lsp::position_encoding e);

/// The legend the tokens' indices refer to, announced once in the capabilities.
[[nodiscard]] lsp::semantic_tokens_legend semantic_tokens_legend();
[[nodiscard]] lsp::semantic_tokens semantic_tokens_of(analysis const& a, lsp::position_encoding e);

/// The hints in `visible`: ` : type` after each unannotated `let`, and `-> type` before the `=>` of each inferred result.
/// Each inserts itself when accepted, except where the type may not be written.
[[nodiscard]] cc::vector<lsp::inlay_hint> inlay_hints_of(analysis const& a, lsp::range visible, lsp::position_encoding e);

/// A quick fix per fix the checker suggests for a diagnostic of the document that touches `around`.
[[nodiscard]] cc::vector<lsp::code_action> code_actions_of(analysis const& a, lsp::range around, lsp::position_encoding e);

/// The declaration of the name at `at` as SGL writes it, with its doc comment; nothing where no resolved name is.
[[nodiscard]] cc::optional<lsp::hover> hover_of(analysis const& a, lsp::position at, lsp::position_encoding e);

/// Where the name at `at` is declared, a prelude file's `sgl-prelude:` document included; nothing where no declaration
/// is, as for a swizzle.
[[nodiscard]] cc::optional<lsp::location> definition_of(analysis const& a, lsp::position at, lsp::position_encoding e);

/// What may be written at `at`, from `sgl::completions_at`.
[[nodiscard]] cc::vector<lsp::completion_item> completions_of(analysis const& a,
                                                              lsp::position at,
                                                              lsp::position_encoding e);

/// The call whose parens hold `at`, its overloads and the argument `at` is in; nothing outside a call.
[[nodiscard]] cc::optional<lsp::signature_help> signature_help_of(analysis const& a,
                                                                  lsp::position at,
                                                                  lsp::position_encoding e);

/// Every place that names what the name at `at` refers to, the declaration where `include_declaration`.
/// A place in the prelude is in its `sgl-prelude:` document.
[[nodiscard]] cc::vector<lsp::location> references_of(analysis const& a,
                                                      lsp::position at,
                                                      bool include_declaration,
                                                      lsp::position_encoding e);

/// The name at `at`, where it may be renamed: declared in the document or a module file, and not by the prelude.
[[nodiscard]] cc::optional<lsp::prepare_rename_result> prepare_rename_of(analysis const& a,
                                                                         lsp::position at,
                                                                         lsp::position_encoding e);

/// The edits that rename what the name at `at` refers to, in every file that names it.
/// An error where `new_name` is no name, is a keyword, or what is named may not be renamed.
[[nodiscard]] lsp::answer<lsp::workspace_edit> rename_of(analysis const& a,
                                                         lsp::position at,
                                                         cc::string_view new_name,
                                                         lsp::position_encoding e);

/// The document's declarations, each with what it holds, from its tree alone.
[[nodiscard]] cc::vector<lsp::document_symbol> document_symbols_of(analysis const& a, lsp::position_encoding e);

/// The runs of lines the document folds: every line that owns deeper ones, and runs of comment lines.
[[nodiscard]] cc::vector<lsp::folding_range> folding_ranges_of(analysis const& a);

/// Every declaration whose name holds `query`'s characters in order, ignoring case, in the open documents and the
/// module directories' files that are not open; an empty query matches every one.
/// Each file is parsed as it stands, so it answers whatever the check thinks of it.
[[nodiscard]] cc::vector<lsp::symbol_information> workspace_symbols_of(lsp::snapshot const& open,
                                                                       cc::string_view query,
                                                                       lsp::position_encoding e);

/// The edit that formats the whole document, by `sgl::format_source`; none where it is formatted already.
/// The blocks around a syntax error stay as written.
[[nodiscard]] lsp::answer<cc::vector<lsp::text_edit>> formatting_of(analysis const& a, lsp::position_encoding e);

} // namespace sgl_lsp

/// One test run of one analysis, and whether it was stopped before its end.
struct sgl_lsp::test_run
{
    cc::shared_ptr<analysis> checked;
    cc::vector<sgl::test::test_result> results;
    bool is_stopped = false;
};

namespace sgl_lsp
{

/// Runs the document's tests one at a time on compute, yielding between them, and stops once `stop` is raised, inside a
/// test too.
[[nodiscard]] cc::shared_async<test_run> run_tests(cc::shared_ptr<analysis> a, cc::shared_ptr<lsp::cancel_flag> stop);

} // namespace sgl_lsp

/// One check or assert of the document, with how often it held and failed over every test that ran.
struct sgl_lsp::check_mark
{
    lsp::range range;
    i32 passed = 0;
    i32 failed = 0;
};

/// The params of `sgl/checkResults`: every site of the document, summed over the tests of `run`, in source order.
/// A test judged by the diagnostics it expects is one more mark, on its `test` keyword: passed when they all occurred.
struct sgl_lsp::check_results_params
{
    cc::string uri;
    i32 version = 0;
    cc::vector<check_mark> marks;
};

namespace sgl_lsp
{

[[nodiscard]] check_results_params check_results_of(test_run const& run, lsp::position_encoding e);

void write_fields(lsp::json::writer& w, check_mark const& v);
void write_fields(lsp::json::writer& w, check_results_params const& v);
} // namespace sgl_lsp
