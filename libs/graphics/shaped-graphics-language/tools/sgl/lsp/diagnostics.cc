#include "features.hh"

#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ast/decl.hh>
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

/// Where the document reaches `module`: the `use` naming it, else its first `use`, since it may reach it through
/// another module; the document's start where it has none.
[[nodiscard]] sgl::source_span use_line_of(sgl_lsp::analysis const& a, cc::string_view module)
{
    auto first = cc::optional<sgl::source_span>();
    for (auto const id : a.ast.at(a.ast.declarations))
    {
        auto const& d = a.ast.at(id);
        auto const* const u = d.node.try_as<sgl::ast::use_decl>();
        if (u == nullptr || !sgl::ast::is_valid(u->path))
            continue;
        auto const where = a.file.at(d.form).where;
        if (!first.has_value())
            first = where;
        if (auto const* const n = a.ast.at(u->path).node.try_as<sgl::ast::name>(); n != nullptr && a.file.text_of(n->where) == module)
            return where;
    }
    return first.has_value() ? first.value() : sgl::source_span{};
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
    // an error in a module the document reaches is shown on the `use` that reaches it, pointing into the module's file
    for (auto const& d : a.diagnostics)
    {
        if (d.file == user || d.file < a.module.prelude_file_count() || d.what.level == sgl::severity::warning)
            continue;
        auto const& module = a.module.file_modules[d.file];
        auto summary = lsp_diagnostic_of(a, d, e);
        summary.related_information.insert_at(0, {.location = {.uri = a.uri_of(d.file), .range = summary.range},
                                                  .message = summary.message});
        summary.range = sgl_lsp::range_of(a, user, use_line_of(a, module), e);
        summary.message = cc::format("module {} does not check: {}", module, summary.message);
        out.push_back(cc::move(summary));
    }
    // a test that did not check has its diagnostics already, and one that expects diagnostics is a mark instead
    for (auto const& r : tests)
        if (!r.is_passed() && r.status != sgl::test::test_status::not_run
            && r.status != sgl::test::test_status::judged_by_diagnostics)
            out.push_back(lsp_diagnostic_of(a, sgl::test::diagnostic_of(a.module, r), e));
    return out;
}
