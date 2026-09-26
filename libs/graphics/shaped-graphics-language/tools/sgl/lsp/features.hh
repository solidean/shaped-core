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

/// A hint ` : type` after each unannotated `let` in `visible`, which inserts itself when accepted.
[[nodiscard]] cc::vector<lsp::inlay_hint> inlay_hints_of(analysis const& a, lsp::range visible, lsp::position_encoding e);

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

/// Runs the document's tests one at a time on compute, yielding between them, and stops early once `stop` is raised.
[[nodiscard]] cc::shared_async<test_run> run_tests(cc::shared_ptr<analysis> a, cc::shared_ptr<lsp::cancel_flag> stop);

} // namespace sgl_lsp

/// One check or assert of the document, with how often it held and failed over every test that ran.
struct sgl_lsp::check_mark
{
    lsp::range range;
    i32 passed = 0;
    i32 failed = 0;
};

/// The params of `sgl/checkResults`: every site of the document, summed over the tests of `run`.
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
