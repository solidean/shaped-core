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
/// Few, since an editor's every keystroke in a module file is another library; each holds the chain it reads.
constexpr isize k_max_libraries = 4;

using link_ptr = cc::shared_ptr<shared_library::chain_link const>;
using unit_ptr = cc::shared_ptr<shared_library::parsed_unit const>;

/// The process's parsed files, by their sources, so a library that changed one file shares the parse of the rest.
cc::mutex<cc::map<cc::hash128, unit_ptr>> g_units;
constexpr isize k_max_units = 1024;

/// The links of every library's chain, by the modules up to theirs: each one's files, their places and their sources.
/// A library whose files change is another key from the first module it changed on, and the links before it are shared.
/// Weak, so a link lives as long as a library or a compile holds it: each holds the whole state up to its module.
cc::mutex<cc::map<cc::hash128, cc::weak_ptr<shared_library::chain_link const>>> g_links;
constexpr isize k_max_links = 256;

link_ptr live_link(cc::hash128 key)
{
    return g_links.lock(
        [&](cc::map<cc::hash128, cc::weak_ptr<shared_library::chain_link const>>& m)
        {
            auto* const p = m.get_ptr(key);
            return p != nullptr ? p->lock() : link_ptr();
        });
}

/// `made` under `key`, or the live link another thread put there meanwhile, which stays the one everyone shares.
link_ptr keep_link(cc::hash128 key, link_ptr made)
{
    return g_links.lock(
        [&](cc::map<cc::hash128, cc::weak_ptr<shared_library::chain_link const>>& m)
        {
            if (auto* const p = m.get_ptr(key); p != nullptr)
                if (auto live = p->lock(); live != nullptr)
                    return live;
            // the entries of links nothing holds any more, once there are many
            if (m.size() >= k_max_links)
            {
                auto live = cc::map<cc::hash128, cc::weak_ptr<shared_library::chain_link const>>();
                for (auto const& [k, w] : m)
                    if (w.lock() != nullptr)
                        live[k] = w;
                m = cc::move(live);
            }
            m[key] = made;
            return made;
        });
}

template <class T>
cc::shared_ptr<T const> found_in(cc::mutex<cc::map<cc::hash128, cc::shared_ptr<T const>>>& cache, cc::hash128 key)
{
    return cache.lock(
        [&](cc::map<cc::hash128, cc::shared_ptr<T const>>& m)
        {
            auto const* const p = m.get_ptr(key);
            return p != nullptr ? *p : cc::shared_ptr<T const>();
        });
}

/// `made` under `key`, or what another thread put there meanwhile, which stays the one everyone shares.
/// Every entry goes once there are `max`; what a library holds stays alive with it.
template <class T>
cc::shared_ptr<T const> kept_in(cc::mutex<cc::map<cc::hash128, cc::shared_ptr<T const>>>& cache,
                                cc::hash128 key,
                                cc::shared_ptr<T const> made,
                                isize max)
{
    return cache.lock(
        [&](cc::map<cc::hash128, cc::shared_ptr<T const>>& m)
        {
            if (auto const* const p = m.get_ptr(key); p != nullptr)
                return *p;
            if (m.size() >= max)
                m.clear();
            m[key] = made;
            return made;
        });
}

/// The last link of `library`'s chain, each link found or checked over the one before; null where a module reports.
link_ptr chain_of(shared_library const& library, sgl::check::checked_prelude const& prelude)
{
    auto const order = sgl::check::library_chain(library.files);
    if (order.empty())
        return {};
    auto before = link_ptr();
    auto key = cc::hash128();
    for (auto const& module : order)
    {
        auto& b = cc::byte_stream_builder::thread_local_scratch();
        b.add_pod(key);
        b.add_string(module.name);
        for (auto const i : module.files)
        {
            b.add_pod(i);
            b.add_string(library.names[i]);
            b.add_string(library.files[i].file.source);
        }
        key = cc::hash128::create(b.written_bytes(), 0);
        auto link = live_link(key);
        if (link == nullptr)
        {
            CC_RECORD_SCOPE("sgl.check_module");
            auto made = sgl::check::check_next_module(before != nullptr ? *before->layer : prelude, library.files,
                                                      module.name);
            if (!made.has_value())
                return {};
            auto own = cc::make_shared<shared_library::chain_link>();
            own->layer = cc::make_shared<sgl::check::checked_prelude const>(cc::move(made.value()));
            own->before = before;
            for (auto const i : module.files)
                own->units.push_back(library.units[i]);
            link = keep_link(key, link_ptr(cc::move(own)));
        }
        before = cc::move(link);
    }
    return before;
}

/// The layer of every module of `library`, made on first need; null where it cannot be made.
layer_ptr library_layer_of(shared_library const& library, sgl::check::checked_prelude const& prelude)
{
    auto const tried = library.checked.lock(
        [&](shared_library::layers& l) -> cc::optional<layer_ptr>
        {
            if (l.has_tried_library)
                return l.library != nullptr ? l.library->layer : layer_ptr();
            return {};
        });
    if (tried.has_value())
        return tried.value();
    auto made = chain_of(library, prelude);
    return library.checked.lock(
        [&](shared_library::layers& l) -> layer_ptr
        {
            // another compile may have made it meanwhile, and the first one is the one everyone shares
            if (!l.has_tried_library)
            {
                l.has_tried_library = true;
                l.library = cc::move(made);
            }
            return l.library != nullptr ? l.library->layer : layer_ptr();
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
    if (auto known = found_in(g_libraries, key); known != nullptr)
        return known;

    CC_RECORD_SCOPE("sgl.parse_library");
    auto made = cc::make_shared<shared_library>();
    for (auto const* f : handed)
    {
        auto const unit_key = cc::hash128::create(f->source.as_bytes(), 0);
        auto unit = found_in(g_units, unit_key);
        if (unit == nullptr)
        {
            auto own = cc::make_shared<shared_library::parsed_unit>();
            own->file = parse(f->source);
            own->ast = ast::build(own->file);
            unit = kept_in(g_units, unit_key, unit_ptr(cc::move(own)), k_max_units);
        }
        made->files.push_back({.file = unit->file, .ast = unit->ast});
        made->units.push_back(cc::move(unit));
        made->names.push_back(cc::string(f->name));
    }
    return kept_in(g_libraries, key, cc::shared_ptr<shared_library const>(cc::move(made)), k_max_libraries);
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
