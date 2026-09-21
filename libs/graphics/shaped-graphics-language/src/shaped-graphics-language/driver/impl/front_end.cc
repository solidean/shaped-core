#include "front_end.hh"

#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/source/format_diagnostic.hh>

sgl::driver::impl::front_end sgl::driver::impl::run_front_end(cc::string_view source, cc::string_view source_name)
{
    auto result = front_end{.prelude = prelude_files(), .source_name = source_name};

    // Both vectors are complete before a `module_file` refers into them.
    for (auto const& p : result.prelude)
        result.files.push_back(parse(p.source));
    result.files.push_back(parse(source));
    for (auto const& f : result.files)
        result.asts.push_back(ast::build(f));

    auto const add = [&](isize file, diagnostic const& d, cc::string_view detail = {})
    {
        if (d.level == severity::warning)
            return;
        result.errors += format_diagnostic(result.name_of(file), result.files[file].source, d, detail);
        result.errors += "\n";
    };
    for (auto i = isize(0); i < result.files.size(); ++i)
    {
        for (auto const& d : result.files[i].diagnostics)
            add(i, d);
        for (auto const& d : result.asts[i].diagnostics)
            add(i, d);
    }

    auto modules = cc::vector<check::module_file>();
    for (auto i = isize(0); i < result.prelude.size(); ++i)
        modules.push_back({.file = result.files[i], .ast = result.asts[i]});
    result.module = check::check(modules, {.file = result.files.back(), .ast = result.asts.back()});
    for (auto const& d : result.module.diagnostics)
        add(d.file, d.what, d.detail);

    return result;
}
