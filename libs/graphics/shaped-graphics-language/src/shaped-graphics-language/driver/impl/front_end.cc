#include "front_end.hh"

#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/source/format_diagnostic.hh>
#include <shaped-graphics-language/test/run_tests.hh>

cc::string sgl::driver::impl::format_located(front_end const& front, check::located_diagnostic const& d)
{
    auto text = format_diagnostic(front.name_of(d.file), front.files[d.file].source, d.what, d.detail);
    text += "\n";
    for (auto const& n : d.notes)
    {
        text += format_note(front.name_of(n.file), front.files[n.file].source, n.where, n.message);
        text += "\n";
    }
    return text;
}

cc::vector<sgl::check::module_file> sgl::driver::impl::module_files_of(front_end const& front)
{
    auto files = cc::vector<check::module_file>();
    for (auto i = isize(0); i < front.files.size(); ++i)
        files.push_back({.file = front.files[i], .ast = front.asts[i]});
    return files;
}

sgl::driver::impl::front_end sgl::driver::impl::run_front_end(cc::string_view source, cc::string_view source_name)
{
    auto result = front_end{.prelude = prelude_files(), .source_name = source_name};

    // Both vectors are complete before a `module_file` refers into them.
    for (auto const& p : result.prelude)
        result.files.push_back(parse(p.source));
    result.files.push_back(parse(source));
    for (auto const& f : result.files)
        result.asts.push_back(ast::build(f));

    auto modules = cc::vector<check::module_file>();
    for (auto i = isize(0); i < result.prelude.size(); ++i)
        modules.push_back({.file = result.files[i], .ast = result.asts[i]});
    result.module = check::check(modules, {.file = result.files.back(), .ast = result.asts.back()});

    // Every phase's diagnostics in one list, so a test's `@expect` can take one of any phase before it is written out.
    auto all = cc::vector<check::located_diagnostic>();
    for (auto i = isize(0); i < result.files.size(); ++i)
    {
        for (auto const& d : result.files[i].diagnostics)
            all.push_back({.what = d, .file = i32(i)});
        for (auto const& d : result.asts[i].diagnostics)
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
