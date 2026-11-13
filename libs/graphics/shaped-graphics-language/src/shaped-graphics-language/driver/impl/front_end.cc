#include "front_end.hh"

#include <clean-core/common/profiling.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/source/format_diagnostic.hh>
#include <shaped-graphics-language/test/run_tests.hh>

using namespace cc::primitive_defines;

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
        names.push_back(m.qualified_name_of(id));
    return names;
}

sgl::driver::impl::front_end sgl::driver::impl::run_front_end(cc::string_view source,
                                                              cc::string_view source_name,
                                                              cc::span<library_file const> library,
                                                              cc::span<check::option_value const> options,
                                                              check::compile_target const& target)
{
    CC_RECORD_SCOPE("sgl.front_end");
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

    auto const* const checked = own >= 0 ? nullptr : checked_prelude();

    // The library is parsed once for the process, and behind the checked prelude its modules are checked once too: a
    // compile that sets one of their options checks that module's link and those after it with its values.
    auto library_files = cc::vector<check::module_file>();
    auto library_names = cc::vector<cc::string_view>();
    CC_RECORD_SCOPE_BEGIN("sgl.parse");
    auto const user = own_file(own >= 0 ? cc::string_view() : source);
    if (own < 0)
        if (auto shared = shared_library_of(library, source_name, source); shared != nullptr)
        {
            library_files = shared->files;
            for (auto const& name : shared->names)
                library_names.push_back(name);
            if (checked != nullptr)
                result.layer = layer_of(*shared, user, options);
            result.library = cc::move(shared);
        }
    CC_RECORD_SCOPE_END("sgl.parse");

    auto modules = cc::vector<check::module_file>();
    for (auto i = isize(0); i < result.prelude.size(); ++i)
        modules.push_back({.file = *result.files[i], .ast = *result.asts[i]});
    CC_RECORD_SCOPE_BEGIN("sgl.check");
    // CHK-426: a layer holds no const of the target, so a compile for any target continues from one
    result.module = result.layer != nullptr ? check::check(*result.layer->layer, library_files, user, options, target)
                  : checked != nullptr
                      ? check::check(*checked, library_files, user, options, target)
                      : check::check(modules, library_files, user, builtins::default_registry(), options, target);
    CC_RECORD_SCOPE_END("sgl.check");

    // The files as the check placed them: the prelude, the library files the source reached, then the source.
    for (auto const i : result.module.reached_library_files)
        result.reached_library_names.push_back(library_names[i]);
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
        auto text = format_located(result, d);
        auto const is_warning = d.what.level == severity::warning;
        if (is_warning)
            text.resize_down_to(text.size() - 1);
        if (!d.entry_points.empty())
            result.entry_diagnostics.push_back(
                {.entry_points = d.entry_points, .is_warning = is_warning, .text = cc::move(text)});
        else if (is_warning)
            result.warnings.push_back(cc::move(text));
        else
            result.errors += text;
    }
    return result;
}
