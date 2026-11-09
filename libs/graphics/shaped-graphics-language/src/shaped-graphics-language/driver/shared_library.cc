#include "shared_library.hh"

#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/map.hh>
#include <clean-core/error/optional.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/driver/prelude.hh>

using namespace cc::primitive_defines;

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

using sgl::shared_library;
using layer_ptr = cc::shared_ptr<sgl::check::checked_prelude const>;

/// The process's libraries, by the names and sources of their files.
cc::mutex<cc::map<cc::hash128, cc::shared_ptr<shared_library const>>> g_libraries;
constexpr isize k_max_libraries = 16;

/// The layer of every module of `library`, made on first need; null where it cannot be made.
layer_ptr library_layer_of(shared_library const& library, sgl::check::checked_prelude const& prelude)
{
    auto const tried = library.checked.lock(
        [&](shared_library::layers& l) -> cc::optional<layer_ptr>
        {
            if (l.has_tried_library)
                return l.library;
            return {};
        });
    if (tried.has_value())
        return tried.value();
    CC_RECORD_SCOPE("sgl.check_library");
    auto made = sgl::check::check_library(prelude, library.files);
    return library.checked.lock(
        [&](shared_library::layers& l) -> layer_ptr
        {
            // another compile may have made it meanwhile, and the first one is the one everyone shares
            if (!l.has_tried_library)
            {
                l.has_tried_library = true;
                if (made.has_value())
                    l.library = cc::make_shared<sgl::check::checked_prelude const>(cc::move(made.value()));
            }
            return l.library;
        });
}
} // namespace

cc::shared_ptr<shared_library const> sgl::shared_library_of(cc::span<library_file const> library,
                                                            cc::string_view source_name)
{
    // A library file named like the source is the source itself, which a module directory holding it hands in again.
    auto handed = cc::vector<library_file const*>();
    for (auto const& f : library)
        if (!is_same_path(f.name, source_name) && may_declare_module(f.source))
            handed.push_back(&f);
    if (handed.empty())
        return {};

    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_pod(i64(handed.size()));
    for (auto const* f : handed)
    {
        b.add_string(f->name);
        b.add_string(f->source);
    }
    auto const key = cc::hash128::create(b.written_bytes(), 0);
    auto const known = g_libraries.lock(
        [&](cc::map<cc::hash128, cc::shared_ptr<shared_library const>>& m)
        {
            auto const* const p = m.get_ptr(key);
            return p != nullptr ? *p : cc::shared_ptr<shared_library const>();
        });
    if (known != nullptr)
        return known;

    CC_RECORD_SCOPE("sgl.parse_library");
    auto made = cc::make_shared<shared_library>();
    for (auto const* f : handed)
    {
        made->parsed.push_back(cc::make_unique<parsed_file>(parse(f->source)));
        made->built.push_back(cc::make_unique<ast::file_ast>(ast::build(*made->parsed.back())));
        made->files.push_back({.file = *made->parsed.back(), .ast = *made->built.back()});
        made->names.push_back(cc::string(f->name));
    }
    auto shared = cc::shared_ptr<shared_library const>(cc::move(made));
    return g_libraries.lock(
        [&](cc::map<cc::hash128, cc::shared_ptr<shared_library const>>& m)
        {
            // another compile may have parsed it meanwhile, and the first one stays the library everyone shares
            if (auto const* const p = m.get_ptr(key); p != nullptr)
                return *p;
            if (m.size() >= k_max_libraries)
                m.clear();
            m[key] = shared;
            return shared;
        });
}

cc::shared_ptr<sgl::check::checked_prelude const> sgl::layer_of(shared_library const& library, check::module_file program)
{
    auto const* const prelude = checked_prelude();
    // a program that reaches no module, or its own module's files, checks beside the library, which is no failure
    if (prelude == nullptr || !check::reaches_modules_to_layer(library.files, program))
        return {};
    if (auto whole = library_layer_of(library, *prelude); whole != nullptr)
        return whole;

    auto const found = library.checked.lock(
        [&](shared_library::layers& l) -> cc::optional<layer_ptr>
        {
            if (l.has_failed)
                return layer_ptr();
            for (auto const& layer : l.made)
                if (check::reaches_the_same_modules(*layer, library.files, program))
                    return layer;
            return {};
        });
    if (found.has_value())
        return found.value();

    CC_RECORD_SCOPE("sgl.check_modules");
    auto made = check::check_modules(*prelude, library.files, program);
    return library.checked.lock(
        [&](shared_library::layers& l) -> layer_ptr
        {
            if (!made.has_value())
            {
                l.has_failed = true;
                return {};
            }
            // another compile may have made the same layer meanwhile, and the first one is the one everyone shares
            for (auto const& layer : l.made)
                if (check::reaches_the_same_modules(*layer, library.files, program))
                    return layer;
            l.made.push_back(cc::make_shared<check::checked_prelude const>(cc::move(made.value())));
            return l.made.back();
        });
}
