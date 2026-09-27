#include "features.hh"

#include <shaped-graphics-language/source/diagnostic.hh>

using namespace cc::primitive_defines;

lsp::range sgl_lsp::range_of(analysis const& a, i32 file, sgl::source_span where, lsp::position_encoding e)
{
    auto const text = a.text_of(file);
    auto const& index = a.index_of(file);
    return {.start = index.position_of(text, where.offset, e), .end = index.position_of(text, where.end(), e)};
}

namespace
{
[[nodiscard]] lsp::diagnostic lsp_diagnostic_of(sgl_lsp::analysis const& a,
                                                sgl::check::located_diagnostic const& d,
                                                lsp::position_encoding e)
{
    auto out = lsp::diagnostic{
        .range = sgl_lsp::range_of(a, d.file, d.what.where, e),
        .severity
        = d.what.level == sgl::severity::warning ? lsp::diagnostic_severity::warning : lsp::diagnostic_severity::error,
        .code = cc::string(sgl::to_string(d.what.kind)),
        .source = "sgl",
        .message = d.detail.empty() ? cc::string(sgl::summary_of(d.what.kind)) : d.detail,
    };
    if (d.what.kind == sgl::diagnostic_kind::unreachable_code)
        out.tags.push_back(lsp::diagnostic_tag::unnecessary);
    for (auto const& n : d.notes)
        out.related_information.push_back({
            .location = {.uri = a.uri_of(n.file), .range = sgl_lsp::range_of(a, n.file, n.where, e)},
            .message = n.message,
        });
    return out;
}
} // namespace

cc::vector<lsp::diagnostic> sgl_lsp::diagnostics_of(analysis const& a,
                                                    cc::span<sgl::test::test_result const> tests,
                                                    lsp::position_encoding e)
{
    auto out = cc::vector<lsp::diagnostic>();
    auto const user = a.user_file();
    // a diagnostic in the prelude is the library's bug, and has no place in this document
    for (auto const& d : a.diagnostics)
        if (d.file == user)
            out.push_back(lsp_diagnostic_of(a, d, e));
    // a test that did not check has its diagnostics already, and one that expects diagnostics is a mark instead
    for (auto const& r : tests)
        if (!r.is_passed() && r.status != sgl::test::test_status::not_run
            && r.status != sgl::test::test_status::judged_by_diagnostics)
            out.push_back(lsp_diagnostic_of(a, sgl::test::diagnostic_of(a.module, r), e));
    return out;
}
