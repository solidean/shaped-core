#include "front_end.hh"

#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/source/format_diagnostic.hh>
#include <shaped-graphics-language/test/run_tests.hh>

cc::string sgl::driver::impl::format_located(front_end const& front, check::located_diagnostic const& d)
{
    auto text = format_diagnostic(front.name_of(d.file), front.files[d.file]->source, d.what, d.detail);
    text += "\n";
    for (auto const& n : d.notes)
    {
        text += format_note(front.name_of(n.file), front.files[n.file]->source, n.where, n.message);
        text += "\n";
    }
    return text;
}

cc::vector<sgl::check::module_file> sgl::driver::impl::module_files_of(front_end const& front)
{
    auto files = cc::vector<check::module_file>();
    for (auto i = isize(0); i < front.files.size(); ++i)
        files.push_back({.file = *front.files[i], .ast = *front.asts[i]});
    return files;
}

cc::vector<cc::string> sgl::driver::impl::option_names_of(check::checked_module const& m, check::flat_entry_point const& e)
{
    auto names = cc::vector<cc::string>();
    for (auto const id : e.options)
        names.push_back(m.at(id).name);
    return names;
}

namespace
{
/// False only where `text` plainly declares no module: its first line that is neither blank nor a `//` comment is
/// something other than a `module` line.
/// A library file without one belongs to no module, so it is left out unparsed; a module directory is mostly programs.
bool may_declare_module(cc::string_view text)
{
    // a byte-order mark, which the parser reads as indentation
    if (text.starts_with("\xEF\xBB\xBF"))
        text = text.subview(3);
    while (!text.empty())
    {
        auto const end = text.find('\n');
        auto line = end < 0 ? text : text.subview({.offset = 0, .size = end});
        text = end < 0 ? cc::string_view() : text.subview(end + 1);
        while (!line.empty() && (line[0] == ' ' || line[0] == '\t' || line[0] == '\r'))
            line = line.subview(1);
        if (line.empty() || line.starts_with("//"))
            continue;
        // a block comment, or anything else this does not read, is left to the parser
        return line.starts_with("module") || line.starts_with("/");
    }
    return false;
}
} // namespace

sgl::driver::impl::front_end sgl::driver::impl::run_front_end(cc::string_view source,
                                                              cc::string_view source_name,
                                                              cc::span<library_file const> library,
                                                              cc::span<check::option_value const> options)
{
    auto result = front_end{.prelude = prelude_files(), .source_name = source_name};
    auto const own = prelude_file_of(source_name);

    // The source, a prelude file it stands in for and the library are this compile's own; every other file is shared.
    auto const own_file = [&](cc::string_view text)
    {
        result.owned_files.push_back(cc::make_unique<parsed_file>(parse(text)));
        result.owned_asts.push_back(cc::make_unique<ast::file_ast>(ast::build(*result.owned_files.back())));
        return check::module_file{.file = *result.owned_files.back(), .ast = *result.owned_asts.back()};
    };
    auto const shared = parsed_prelude();
    for (auto i = isize(0); i < result.prelude.size(); ++i)
        if (i == own)
        {
            auto const f = own_file(source);
            result.files.push_back(&f.file);
            result.asts.push_back(&f.ast);
        }
        else
        {
            result.files.push_back(&shared[i].file);
            result.asts.push_back(&shared[i].ast);
        }

    // A library file named like the source is the source itself, which a module directory holding it hands in again.
    auto library_files = cc::vector<check::module_file>();
    auto library_names = cc::vector<cc::string_view>();
    if (own < 0)
        for (auto const& f : library)
            if (!is_same_path(f.name, source_name) && may_declare_module(f.source))
            {
                library_files.push_back(own_file(f.source));
                library_names.push_back(f.name);
            }
    auto const user = own_file(own >= 0 ? cc::string_view() : source);

    auto modules = cc::vector<check::module_file>();
    for (auto i = isize(0); i < result.prelude.size(); ++i)
        modules.push_back({.file = *result.files[i], .ast = *result.asts[i]});
    auto const* const checked = own >= 0 ? nullptr : checked_prelude();
    result.module = checked != nullptr
                      ? check::check(*checked, library_files, user, options)
                      : check::check(modules, library_files, user, builtins::default_registry(), options);

    // The files as the check placed them: the prelude, the library files the source reached, then the source.
    for (auto const i : result.module.library_files)
    {
        result.files.push_back(&library_files[i].file);
        result.asts.push_back(&library_files[i].ast);
        result.library_names.push_back(library_names[i]);
    }
    result.files.push_back(&user.file);
    result.asts.push_back(&user.ast);
    result.program = own >= 0 ? own : i32(result.files.size()) - 1;

    // Every phase's diagnostics in one list, so a test's `@expect` can take one of any phase before it is written out.
    auto all = cc::vector<check::located_diagnostic>();
    for (auto i = isize(0); i < result.files.size(); ++i)
    {
        for (auto const& d : result.files[i]->diagnostics)
            all.push_back({.what = d, .file = i32(i)});
        for (auto const& d : result.asts[i]->diagnostics)
            all.push_back({.what = d, .file = i32(i)});
    }
    for (auto const& d : result.module.diagnostics)
        all.push_back(d);
    test::contain_expected(result.module, all);

    for (auto const& d : all)
    {
        auto& into = d.what.level == severity::warning ? result.warnings : result.errors;
        into += format_located(result, d);
    }
    return result;
}
