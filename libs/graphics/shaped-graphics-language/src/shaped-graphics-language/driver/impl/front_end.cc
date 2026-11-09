#include "front_end.hh"

#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/map.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/builtins/registry.hh>
#include <shaped-graphics-language/check/artifact.hh>
#include <shaped-graphics-language/check/check.hh>
#include <shaped-graphics-language/driver/prelude.hh>
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

using sgl::driver::impl::parsed_library;

/// The process's parsed libraries, by the names and sources of their files.
/// A library whose files change is another key, and every one goes once there are many, so hot reloading cannot grow
/// it without bound.
cc::mutex<cc::map<cc::hash128, cc::shared_ptr<parsed_library>>> g_libraries;
constexpr isize k_max_libraries = 16;

/// The library of these files, parsed once for the process.
cc::shared_ptr<parsed_library> library_of(cc::span<sgl::library_file const* const> files)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(i64(files.size()));
    for (auto const* f : files)
    {
        b.add_string(f->name);
        b.add_string(f->source);
    }
    auto const key = cc::hash128::create(b.written_bytes(), 0);
    auto const known = g_libraries.lock(
        [&](cc::map<cc::hash128, cc::shared_ptr<parsed_library>>& m)
        {
            auto const* const p = m.get_ptr(key);
            return p != nullptr ? *p : cc::shared_ptr<parsed_library>();
        });
    if (known != nullptr)
        return known;

    auto made = cc::make_shared<parsed_library>();
    for (auto const* f : files)
    {
        made->parsed.push_back(cc::make_unique<sgl::parsed_file>(sgl::parse(f->source)));
        made->built.push_back(cc::make_unique<sgl::ast::file_ast>(sgl::ast::build(*made->parsed.back())));
        made->files.push_back({.file = *made->parsed.back(), .ast = *made->built.back()});
        made->names.push_back(cc::string(f->name));
    }
    return g_libraries.lock(
        [&](cc::map<cc::hash128, cc::shared_ptr<parsed_library>>& m)
        {
            // another compile may have parsed it meanwhile, and the first one stays the library everyone shares
            if (auto const* const p = m.get_ptr(key); p != nullptr)
                return *p;
            if (m.size() >= k_max_libraries)
                m.clear();
            m[key] = made;
            return made;
        });
}

using layer_ptr = cc::shared_ptr<sgl::check::checked_prelude const>;

/// The layer of `library`'s modules that `program` reaches, made on first need; null where none can be made.
layer_ptr layer_of(parsed_library& library, sgl::check::checked_prelude const& prelude, sgl::check::module_file program)
{
    auto const found = library.checked.lock(
        [&](parsed_library::layers& l) -> cc::optional<layer_ptr>
        {
            if (l.has_failed)
                return layer_ptr();
            for (auto const& layer : l.made)
                if (sgl::check::reaches_the_same_modules(*layer, library.files, program))
                    return layer;
            return {};
        });
    if (found.has_value())
        return found.value();
    // a program that reaches no module, or its own module's files, checks beside the library, which is no failure
    if (!sgl::check::reaches_modules_to_layer(library.files, program))
        return {};

    CC_RECORD_SCOPE("sgl.check_modules");
    // the compilation model's artifacts: a layer an earlier process checked, kept in the host's store by its content
    auto const key = sgl::impl::artifact_key();
    auto const id = sgl::check::modules_artifact_id(key, prelude, library.files, program);
    auto made = cc::optional<sgl::check::checked_prelude>();
    if (id.has_value())
        if (auto const bytes = sgl::impl::load_artifact_bytes(id.value()); bytes.has_value())
            made = sgl::check::load_modules_artifact(bytes.value(), key, prelude, library.files, program,
                                                     sgl::builtins::default_registry());
    if (!made.has_value())
    {
        made = sgl::check::check_modules(prelude, library.files, program);
        if (made.has_value() && id.has_value())
            if (auto const bytes = sgl::check::save_artifact(made.value(), key); bytes.has_value())
                sgl::impl::save_artifact_bytes(id.value(), bytes.value());
    }
    return library.checked.lock(
        [&](parsed_library::layers& l) -> layer_ptr
        {
            if (!made.has_value())
            {
                l.has_failed = true;
                return {};
            }
            // another compile may have made the same layer meanwhile, and the first one is the one everyone shares
            for (auto const& layer : l.made)
                if (sgl::check::reaches_the_same_modules(*layer, library.files, program))
                    return layer;
            l.made.push_back(cc::make_shared<sgl::check::checked_prelude const>(cc::move(made.value())));
            return l.made.back();
        });
}
} // namespace

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

    // A library file named like the source is the source itself, which a module directory holding it hands in again.
    auto handed = cc::vector<library_file const*>();
    if (own < 0)
        for (auto const& f : library)
            if (!is_same_path(f.name, source_name) && may_declare_module(f.source))
                handed.push_back(&f);
    auto const* const checked = own >= 0 ? nullptr : checked_prelude();

    // Behind the checked prelude, the library is parsed once for the process, and its modules are checked once per set
    // of them a program reaches: `check_modules` makes that layer, and a compile that sets options checks them itself.
    auto library_files = cc::vector<check::module_file>();
    auto library_names = cc::vector<cc::string_view>();
    CC_RECORD_SCOPE_BEGIN("sgl.parse");
    auto const user = own_file(own >= 0 ? cc::string_view() : source);
    if (checked != nullptr && !handed.empty())
    {
        auto shared_library = library_of(handed);
        library_files = shared_library->files;
        for (auto const& name : shared_library->names)
            library_names.push_back(name);
        if (options.empty())
            result.layer = layer_of(*shared_library, *checked, user);
        result.library = cc::move(shared_library);
    }
    else
        for (auto const* f : handed)
        {
            library_files.push_back(own_file(f->source));
            library_names.push_back(f->name);
        }
    CC_RECORD_SCOPE_END("sgl.parse");

    auto modules = cc::vector<check::module_file>();
    for (auto i = isize(0); i < result.prelude.size(); ++i)
        modules.push_back({.file = *result.files[i], .ast = *result.asts[i]});
    CC_RECORD_SCOPE_BEGIN("sgl.check");
    // CHK-426: a layer holds no const of the target, so a compile for any target continues from one
    result.module = result.layer != nullptr ? check::check(*result.layer, library_files, user, options, target)
                  : checked != nullptr
                      ? check::check(*checked, library_files, user, options, target)
                      : check::check(modules, library_files, user, builtins::default_registry(), options, target);
    CC_RECORD_SCOPE_END("sgl.check");

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
