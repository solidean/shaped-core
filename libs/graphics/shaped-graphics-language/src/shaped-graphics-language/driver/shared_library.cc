#include "shared_library.hh"

#include <clean-core/bytes/hash128.hh>
#include <clean-core/common/profiling.hh>
#include <clean-core/container/byte_stream_builder.hh>
#include <clean-core/container/map.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/ast/build.hh>
#include <shaped-graphics-language/check/artifact.hh>
#include <shaped-graphics-language/driver/compile_to_text.hh>
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

/// Whether `text` has a line `use name`, as module `name` is reached; a comment or a string that reads so is a harmless
/// yes, since a module no `use` reaches is never checked.
bool may_use_module(cc::string_view text, cc::string_view name)
{
    while (!text.empty())
    {
        auto const end = text.find('\n');
        auto line = end < 0 ? text : text.subview({.offset = 0, .size = end});
        text = end < 0 ? cc::string_view() : text.subview(end + 1);
        while (!line.empty() && (line[0] == ' ' || line[0] == '\t'))
            line = line.subview(1);
        if (!line.starts_with("use "))
            continue;
        line = line.subview(4);
        while (!line.empty() && line[0] == ' ')
            line = line.subview(1);
        if (line.starts_with(name)
            && (line.size() == name.size() || line[name.size()] == ' ' || line[name.size()] == '\r'))
            return true;
    }
    return false;
}

using sgl::shared_library;
using layer_ptr = cc::shared_ptr<sgl::check::checked_prelude const>;

/// The process's libraries, by the names and sources of their files.
cc::mutex<cc::map<cc::hash128, cc::shared_ptr<shared_library const>>> g_libraries;
/// Few, since an editor's every keystroke in a module file is another library; each holds the chain it reads.
constexpr isize k_max_libraries = 4;
/// The library made or found last under each list of names, which the next one made under the same names is an edit of.
cc::mutex<cc::map<cc::hash128, cc::shared_ptr<shared_library const>>> g_last_libraries;
constexpr isize k_max_name_lists = 16;

using link_ptr = shared_library::link_ptr;
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

/// The key a host keeps `module`'s link of `library` under: the compiler's build, the module's name, and its files'
/// sources, in their order; not their names, which no link holds.
cc::string artifact_key_of(shared_library const& library, sgl::check::chained_module const& module)
{
    auto& b = cc::byte_stream_builder::thread_local_scratch();
    b.add_string(sgl::compiler_fingerprint());
    b.add_string(module.name);
    for (auto const i : module.files)
        b.add_string(library.files[i].file.source);
    auto const h = cc::hash128::create(b.written_bytes(), 0);
    return cc::format("sgl-link-{:016x}{:016x}", h.high, h.low);
}

/// What hosts keep of the process's links: the artifacts handed in, and those made since a host last took them.
struct artifact_shelf
{
    bool is_keeping = false;
    cc::map<cc::string, cc::vector<byte>> offered;
    cc::vector<sgl::module_artifact> made;
};
cc::mutex<artifact_shelf> g_artifacts;
/// Few, since an artifact is a module's whole own part; every one goes once there are more.
constexpr isize k_max_offered = 256;

/// The link `module` adds over `base` from the artifact a host handed in under `key`, where one loads there.
cc::optional<sgl::check::checked_prelude> loaded_link(shared_library const& library,
                                                      sgl::check::checked_prelude const& base,
                                                      cc::string_view module,
                                                      cc::string_view key)
{
    // the bytes are copied out, so a load never holds the shelf
    auto const bytes = g_artifacts.lock(
        [&](artifact_shelf& s) -> cc::optional<cc::vector<byte>>
        {
            if (auto const* const p = s.offered.get_ptr(cc::string(key)); p != nullptr)
                return *p;
            return {};
        });
    if (!bytes.has_value())
        return {};
    CC_RECORD_SCOPE("sgl.load_module");
    return sgl::check::load_link_artifact(bytes.value(), key, base, library.files, module);
}

/// The last link of `library`'s chain, each link found or checked over the one before; null where a module reports.
/// Each link is checked with the values `options` give its module's options, which are part of its key, so the links
/// before the first module a compile sets an option of are the ones every compile shares.
/// A link not made yet is moved rather than checked where it may be: `moved_from`'s link of its module, checked over
/// links that differ from this chain's since (the compilation model's early cutoff), or a host's artifact of it.
link_ptr chain_of(shared_library const& library,
                  sgl::check::checked_prelude const& prelude,
                  cc::span<sgl::check::option_value const> options,
                  link_ptr const& moved_from)
{
    // the modules of the files being edited go late
    auto const order = sgl::check::library_chain(library.files, library.late_files);
    if (order.empty())
        return {};
    auto old_links = cc::map<cc::string, link_ptr>();
    for (auto link = moved_from; link != nullptr; link = link->before)
        old_links[link->module] = link;
    // an old link of a module is this chain's only where it was checked from the very sources this library holds
    auto const holds_same_files = [&](shared_library::chain_link const& old, sgl::check::chained_module const& module)
    {
        if (old.units.size() != module.files.size())
            return false;
        for (auto i = isize(0); i < old.units.size(); ++i)
            if (old.units[i] != library.units[module.files[i]]
                && old.units[i]->file.source != library.files[module.files[i]].file.source)
                return false;
        return true;
    };
    auto const is_keeping = g_artifacts.lock([](artifact_shelf& s) { return s.is_keeping; });

    auto before = link_ptr();
    auto key = cc::hash128();
    for (auto const& module : order)
    {
        auto const own_options = sgl::check::options_of_module(options, module.name);
        auto& b = cc::byte_stream_builder::thread_local_scratch();
        b.add_pod(key);
        b.add_string(module.name);
        for (auto const& o : own_options)
        {
            b.add_string(o.name);
            b.add_string(o.value);
        }
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
            auto const& base = before != nullptr ? *before->layer : prelude;
            auto made = cc::optional<sgl::check::checked_prelude>();
            // the old link of the module, where the links before it differ from this chain's
            if (auto const* const old = old_links.get_ptr(module.name);
                own_options.empty() && old != nullptr && (*old)->before != before && holds_same_files(**old, module))
            {
                CC_RECORD_SCOPE("sgl.relocate_module");
                made = sgl::check::relocate_link(*(*old)->layer,
                                                 (*old)->before != nullptr ? *(*old)->before->layer : prelude, base);
            }
            auto const artifact_key
                = own_options.empty() && options.empty() ? artifact_key_of(library, module) : cc::string();
            if (!made.has_value() && !artifact_key.empty())
                made = loaded_link(library, base, module.name, artifact_key);
            if (!made.has_value())
            {
                CC_RECORD_SCOPE("sgl.check_module");
                made = sgl::check::check_next_module(base, library.files, module.name, own_options);
                if (made.has_value() && is_keeping && !artifact_key.empty())
                    if (auto bytes = sgl::check::save_link_artifact(made.value(), base, artifact_key); bytes.has_value())
                        g_artifacts.lock(
                            [&](artifact_shelf& s)
                            {
                                // a host that never takes them keeps the newest few
                                if (s.made.size() >= k_max_offered)
                                    s.made.remove_at(0);
                                s.made.push_back({.key = artifact_key, .bytes = cc::move(bytes.value())});
                            });
            }
            if (!made.has_value())
                return {};
            auto own = cc::make_shared<shared_library::chain_link>();
            own->module = module.name;
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

/// The chain of every module of `library` at its options' defaults, made on first need; null where it cannot be made.
link_ptr library_chain_of(shared_library const& library, sgl::check::checked_prelude const& prelude)
{
    auto const tried = library.checked.lock(
        [&](shared_library::layers& l) -> cc::optional<link_ptr>
        {
            if (l.has_tried_library)
                return l.library;
            return {};
        });
    if (tried.has_value())
        return tried.value();
    // an edit's links move from the library made before it under the same names
    auto const previous = library.previous.lock();
    auto const moved_from
        = previous != nullptr ? previous->checked.lock([](shared_library::layers& l) { return l.library; }) : link_ptr();
    auto made = chain_of(library, prelude, {}, moved_from);
    return library.checked.lock(
        [&](shared_library::layers& l) -> link_ptr
        {
            // another compile may have made it meanwhile, and the first one is the one everyone shares
            if (!l.has_tried_library)
            {
                l.has_tried_library = true;
                l.library = cc::move(made);
            }
            return l.library;
        });
}

/// How many chains with options a library keeps alive: the permutations a host compiles, most recent last.
constexpr isize k_max_option_chains = 32;
} // namespace

cc::shared_ptr<shared_library const> sgl::shared_library_of(cc::span<library_file const> library,
                                                            cc::string_view source_name,
                                                            cc::string_view source)
{
    // A library file named like the source is the source itself, which a module directory holding it hands in again.
    auto handed = cc::vector<library_file const*>();
    for (auto const& f : library)
        if (!is_same_path(f.name, source_name) && may_declare_module(f.source))
            handed.push_back(&f);
    // a module SGL ships stands behind the caller's where a file uses it, unless the source is it or the caller hands it in
    auto const shipped = shipped_library();
    for (auto i = isize(0); i < shipped.size(); ++i)
    {
        auto const name = shipped[i].name.subview({.offset = 0, .size = shipped[i].name.size() - 4});
        auto is_used = may_use_module(source, name);
        for (auto const* f : handed)
            is_used = is_used || may_use_module(f->source, name);
        auto is_handed = shipped_library_file_of(source_name) == i;
        for (auto const& f : library)
            is_handed = is_handed || shipped_library_file_of(f.name) == i;
        if (is_used && !is_handed)
            handed.push_back(&shipped[i]);
    }
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
    // what changed from the library last used under the same names is what an editor is typing in
    auto& names = cc::byte_stream_builder::thread_local_scratch();
    for (auto const* f : handed)
        names.add_string(f->name);
    auto const names_key = cc::hash128::create(names.written_bytes(), 0);
    auto const remember = [&](cc::shared_ptr<shared_library const> const& library)
    {
        g_last_libraries.lock(
            [&](cc::map<cc::hash128, cc::shared_ptr<shared_library const>>& m)
            {
                if (m.size() >= k_max_name_lists && !m.contains(names_key))
                    m.clear();
                m[names_key] = library;
            });
    };
    // a library found again is the one the next edit starts from, as much as one just made
    if (auto known = found_in(g_libraries, key); known != nullptr)
    {
        remember(known);
        return known;
    }

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
    if (auto const last = found_in(g_last_libraries, names_key); last != nullptr)
    {
        for (auto i = isize(0); i < made->files.size(); ++i)
            if (last->units[i] != made->units[i])
                made->late_files.push_back(i32(i));
        if (made->late_files.empty())
            made->late_files = last->late_files;
        made->previous = last;
    }
    auto shared = kept_in(g_libraries, key, cc::shared_ptr<shared_library const>(cc::move(made)), k_max_libraries);
    remember(shared);
    return shared;
}

sgl::shared_library::link_ptr sgl::layer_of(shared_library const& library,
                                            check::module_file program,
                                            cc::span<check::option_value const> options)
{
    auto const* const prelude = checked_prelude();
    // a program that reaches no module, or its own module's files, checks beside the library, which is no failure
    if (prelude == nullptr || !check::reaches_modules_to_layer(library.files, program))
        return {};
    if (auto whole = library_chain_of(library, *prelude); whole != nullptr)
    {
        if (check::layer_serves_options(*whole->layer, options))
            return whole;
        // A compile that sets a module's option: the chain with its values, from the first module it sets one of.
        // A value its option does not take reports, so that compile checks beside the library and reports it.
        CC_RECORD_SCOPE("sgl.check_options");
        // its links after that module move from the defaults' chain where they may
        auto with = chain_of(library, *prelude, options, whole);
        if (with == nullptr || !check::layer_serves_options(*with->layer, options))
            return {};
        library.checked.lock(
            [&](shared_library::layers& l)
            {
                for (auto const& kept : l.with_options)
                    if (kept == with)
                        return;
                if (l.with_options.size() >= k_max_option_chains)
                    l.with_options.remove_at(0);
                l.with_options.push_back(with);
            });
        return with;
    }

    // A layer of the modules a program reaches, where the library makes no chain; checked at the defaults only.
    auto const found = library.checked.lock(
        [&](shared_library::layers& l) -> cc::optional<link_ptr>
        {
            if (l.has_failed)
                return link_ptr();
            for (auto const& link : l.made)
                if (check::reaches_the_same_modules(*link->layer, library.files, program))
                    return link;
            return {};
        });
    if (found.has_value())
        return found.value() != nullptr && check::layer_serves_options(*found.value()->layer, options) ? found.value()
                                                                                                       : link_ptr();

    CC_RECORD_SCOPE("sgl.check_modules");
    auto made = check::check_modules(*prelude, library.files, program);
    auto const link = library.checked.lock(
        [&](shared_library::layers& l) -> link_ptr
        {
            if (!made.has_value())
            {
                l.has_failed = true;
                return {};
            }
            // another compile may have made the same layer meanwhile, and the first one is the one everyone shares
            for (auto const& kept : l.made)
                if (check::reaches_the_same_modules(*kept->layer, library.files, program))
                    return kept;
            auto own = cc::make_shared<shared_library::chain_link>();
            own->layer = cc::make_shared<check::checked_prelude const>(cc::move(made.value()));
            l.made.push_back(link_ptr(cc::move(own)));
            return l.made.back();
        });
    return link != nullptr && check::layer_serves_options(*link->layer, options) ? link : link_ptr();
}

cc::vector<cc::string> sgl::module_artifact_keys(cc::span<library_file const> library,
                                                 cc::string_view source_name,
                                                 cc::string_view source)
{
    auto const shared = shared_library_of(library, source_name, source);
    if (shared == nullptr)
        return {};
    auto keys = cc::vector<cc::string>();
    for (auto const& module : check::library_chain(shared->files))
        keys.push_back(artifact_key_of(*shared, module));
    return keys;
}

void sgl::keep_module_artifacts(bool is_on)
{
    g_artifacts.lock(
        [&](artifact_shelf& s)
        {
            s.is_keeping = is_on;
            if (!is_on)
                s = {};
        });
}

void sgl::offer_module_artifacts(cc::span<module_artifact const> artifacts)
{
    g_artifacts.lock(
        [&](artifact_shelf& s)
        {
            for (auto const& a : artifacts)
            {
                if (s.offered.size() >= k_max_offered && !s.offered.contains(a.key))
                    s.offered.clear();
                s.offered[a.key] = a.bytes;
            }
        });
}

cc::vector<sgl::module_artifact> sgl::take_module_artifacts()
{
    return g_artifacts.lock([](artifact_shelf& s) { return cc::move(s.made); });
}
