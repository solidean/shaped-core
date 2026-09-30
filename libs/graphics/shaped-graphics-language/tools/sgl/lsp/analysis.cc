#include "analysis.hh"

#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/driver/prelude.hh>
#include <shaped-graphics-language/test/run_tests.hh>

sgl_lsp::prelude const& sgl_lsp::the_prelude()
{
    static auto const p = []
    {
        auto out = prelude{.files = sgl::parsed_prelude()};
        for (auto const& f : sgl::prelude_files())
        {
            out.names.push_back(cc::string(f.name));
            out.indices.push_back(lsp::text_index(f.source));
        }
        return out;
    }();
    return p;
}

cc::vector<sgl::check::module_file> sgl_lsp::analysis::module_files() const
{
    auto const& p = the_prelude();
    auto out = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < p.files.size(); ++i)
        out.push_back(i == own_file ? sgl::check::module_file{.file = file, .ast = ast}
                                    : sgl::check::module_file{.file = p.files[i].file, .ast = p.files[i].ast});
    if (own_file < p.files.size())
        out.push_back({.file = empty_file, .ast = empty_ast});
    else
        out.push_back({.file = file, .ast = ast});
    return out;
}

cc::string_view sgl_lsp::analysis::text_of(i32 f) const
{
    auto const& p = the_prelude();
    if (f == own_file)
        return document->text;
    return f < p.files.size() ? cc::string_view(p.files[f].file.source) : cc::string_view();
}

lsp::text_index const& sgl_lsp::analysis::index_of(i32 f) const
{
    auto const& p = the_prelude();
    return f == own_file || f >= p.files.size() ? document->index : p.indices[f];
}

cc::string sgl_lsp::analysis::uri_of(i32 f) const
{
    auto const& p = the_prelude();
    return f == own_file || f >= p.files.size() ? document->uri : p.uri_of(f);
}

cc::shared_ptr<sgl_lsp::analysis> sgl_lsp::analyze(cc::shared_ptr<lsp::document> document)
{
    auto const& p = the_prelude();
    auto a = cc::make_shared<analysis>();
    a->document = cc::move(document);
    a->file = sgl::parse(a->document->text);
    a->ast = sgl::ast::build(a->file);
    a->empty_file = sgl::parse("");
    a->empty_ast = sgl::ast::build(a->empty_file);
    auto const own = sgl::prelude_file_of(a->document->uri);
    a->own_file = own >= 0 ? own : i32(p.files.size());

    auto files = a->module_files();
    auto const behind = files.back();
    files.remove_back();
    auto const* const checked = own >= 0 ? nullptr : sgl::checked_prelude();
    a->module = checked != nullptr ? sgl::check::check(*checked, behind) : sgl::check::check(files, behind);

    auto const user = a->user_file();
    for (auto const& d : a->file.diagnostics)
        a->diagnostics.push_back({.what = d, .file = user});
    for (auto const& d : a->ast.diagnostics)
        a->diagnostics.push_back({.what = d, .file = user});
    for (auto const& d : a->module.diagnostics)
        a->diagnostics.push_back(d);
    sgl::test::contain_expected(a->module, a->diagnostics);
    return a;
}

sgl_lsp::analysis_cache::handle sgl_lsp::analysis_cache::of(lsp::snapshot const& snap, cc::string_view uri)
{
    auto document = snap.share(uri);
    if (!document)
        return {};
    auto& e = _entries[cc::string(uri)];
    if (e.document.get() != document.get() || !e.analysis)
    {
        e.document = document;
        e.analysis = cc::make_async_lazy([document] { return analyze(document); });
    }
    return e.analysis;
}
