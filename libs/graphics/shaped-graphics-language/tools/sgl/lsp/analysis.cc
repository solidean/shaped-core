#include "analysis.hh"

#include <clean-core/string/format.hh>
#include <clean-core/string/uri.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/driver/prelude.hh>
#include <shaped-graphics-language/test/run_tests.hh>

namespace
{
cc::mutex<cc::vector<cc::string>>& configured_module_dirs()
{
    static auto dirs = cc::mutex<cc::vector<cc::string>>();
    return dirs;
}

/// The `file://` uri of an absolute path, which a location in a module's file names.
/// Percent-encoded as a uri path, so a space or `#` stays in the path.
cc::string uri_of_path(cc::string_view path)
{
    auto slashed = cc::string(path);
    slashed.replace_all("\\", "/");
    auto const encoded = cc::percent_encode(slashed, cc::uri_component::path);
    return encoded.starts_with("/") ? cc::format("file://{}", encoded) : cc::format("file:///{}", encoded);
}
} // namespace

void sgl_lsp::set_module_dirs(cc::vector<cc::string> dirs)
{
    configured_module_dirs().lock([&](cc::vector<cc::string>& d) { d = cc::move(dirs); });
}

cc::vector<cc::string> sgl_lsp::module_dirs()
{
    return configured_module_dirs().lock([](cc::vector<cc::string> const& d) { return d; });
}

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
    // the library files the document reached, as the check placed them
    for (auto i = isize(0); i < module.library_files.size(); ++i)
        out.push_back({.file = *library_parsed[i], .ast = *library_asts[i]});
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
    if (f < p.files.size())
        return cc::string_view(p.files[f].file.source);
    auto const reached = f - p.files.size();
    return reached < module.library_files.size() ? cc::string_view(library.sources[module.library_files[reached]])
                                                 : cc::string_view();
}

lsp::text_index const& sgl_lsp::analysis::index_of(i32 f) const
{
    auto const& p = the_prelude();
    if (f == own_file)
        return document->index;
    if (f < p.files.size())
        return p.indices[f];
    auto const reached = f - p.files.size();
    return reached < library_indices.size() ? library_indices[reached] : document->index;
}

cc::string sgl_lsp::analysis::uri_of(i32 f) const
{
    auto const& p = the_prelude();
    if (f == own_file)
        return document->uri;
    if (f < p.files.size())
        return p.uri_of(f);
    auto const reached = f - p.files.size();
    return reached < module.library_files.size() ? uri_of_path(library.names[module.library_files[reached]])
                                                 : document->uri;
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

    // The prelude's own files are checked alone; any other document behind the modules of the configured directories,
    // its own file among them left out.
    auto library = cc::vector<sgl::check::module_file>();
    auto parsed_of = cc::vector<isize>();
    if (own < 0)
    {
        if (auto read = sgl_tool::read_module_dirs(module_dirs()); read.has_value())
            a->library = cc::move(read.value());
        for (auto i = isize(0); i < a->library.files.size(); ++i)
        {
            if (sgl::is_same_path(a->library.names[i], a->document->uri))
                continue;
            a->library_parsed.push_back(cc::make_unique<sgl::parsed_file>(sgl::parse(a->library.sources[i])));
            a->library_asts.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*a->library_parsed.back())));
            library.push_back({.file = *a->library_parsed.back(), .ast = *a->library_asts.back()});
            parsed_of.push_back(i);
        }
    }

    auto const& prelude_files = p.files;
    auto files = cc::vector<sgl::check::module_file>();
    for (auto i = isize(0); i < prelude_files.size(); ++i)
        files.push_back(i == own ? sgl::check::module_file{.file = a->file, .ast = a->ast}
                                 : sgl::check::module_file{.file = prelude_files[i].file, .ast = prelude_files[i].ast});
    auto const behind = own >= 0 ? sgl::check::module_file{.file = a->empty_file, .ast = a->empty_ast}
                                 : sgl::check::module_file{.file = a->file, .ast = a->ast};
    auto const* const checked = own >= 0 ? nullptr : sgl::checked_prelude();
    a->module = checked != nullptr ? sgl::check::check(*checked, library, behind)
                                   : sgl::check::check(files, library, behind, sgl::builtins::default_registry());

    // what the check reached, renumbered from the parsed files to the library read, and in the check's order
    auto reached_parsed = cc::vector<cc::unique_ptr<sgl::parsed_file>>();
    auto reached_asts = cc::vector<cc::unique_ptr<sgl::ast::file_ast>>();
    for (auto& index : a->module.library_files)
    {
        reached_parsed.push_back(cc::move(a->library_parsed[index]));
        reached_asts.push_back(cc::move(a->library_asts[index]));
        index = i32(parsed_of[index]);
        a->library_indices.push_back(lsp::text_index(a->library.sources[index]));
    }
    a->library_parsed = cc::move(reached_parsed);
    a->library_asts = cc::move(reached_asts);
    if (own < 0)
        a->own_file = a->module.program_file();

    auto const user = a->user_file();
    for (auto const& d : a->file.diagnostics)
        a->diagnostics.push_back({.what = d, .file = user});
    for (auto const& d : a->ast.diagnostics)
        a->diagnostics.push_back({.what = d, .file = user});
    // a reached module's own parse errors, which the check did not see
    for (auto i = isize(0); i < a->library_parsed.size(); ++i)
    {
        auto const file = a->module.prelude_file_count() + i32(i);
        for (auto const& d : a->library_parsed[i]->diagnostics)
            a->diagnostics.push_back({.what = d, .file = file});
        for (auto const& d : a->library_asts[i]->diagnostics)
            a->diagnostics.push_back({.what = d, .file = file});
    }
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
