#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/string/format.hh>
#include <shaped-graphics-language/check/impl/archive.hh>
#include <shaped-graphics-language/check/impl/fields.hh>
#include <shaped-graphics-language/check/impl/relocate.hh>

#include <type_traits>

using namespace sgl;
using namespace sgl::check;
using namespace sgl::check::impl;


namespace
{
using size_field = isize table_sizes::*;

/// Every list of `table_sizes`, which a moved part shifts each of on its own.
constexpr size_field k_tables[] = {
    &table_sizes::symbols,
    &table_sizes::types,
    &table_sizes::members,
    &table_sizes::enum_cases,
    &table_sizes::functions,
    &table_sizes::parameters,
    &table_sizes::bindings,
    &table_sizes::binding_lists,
    &table_sizes::type_lists,
    &table_sizes::samplers,
    &table_sizes::pipelines,
    &table_sizes::pipeline_settings,
    &table_sizes::constants,
    &table_sizes::call_records,
    &table_sizes::written_arguments,
    &table_sizes::call_slots,
    &table_sizes::function_instances,
    &table_sizes::target_texts,
    &table_sizes::literals,
    &table_sizes::function_arguments,
};

/// The list a range of `T` lies in, where it is one an id of another layer may name; none for a tree's own lists and
/// a file's AST.
template <class T>
constexpr size_field table_of_range()
{
    if constexpr (std::is_same_v<T, type_id>)
        return &table_sizes::type_lists;
    else if constexpr (std::is_same_v<T, symbol_id>)
        return &table_sizes::binding_lists;
    else if constexpr (std::is_same_v<T, member_info>)
        return &table_sizes::members;
    else if constexpr (std::is_same_v<T, enum_case_info>)
        return &table_sizes::enum_cases;
    else if constexpr (std::is_same_v<T, parameter>)
        return &table_sizes::parameters;
    else if constexpr (std::is_same_v<T, pipeline_setting>)
        return &table_sizes::pipeline_settings;
    else if constexpr (std::is_same_v<T, written_argument>)
        return &table_sizes::written_arguments;
    else if constexpr (std::is_same_v<T, i32>)
        return &table_sizes::call_slots;
    else
        return nullptr;
}

/// The ids of a relocated link, mapped through the parts that moved.
struct relocator
{
    cc::span<moved_part const> moved;
    /// Set once an id names what a changed link no longer holds.
    bool failed = false;

    /// Position `x` of list `t` where the parts moved it; `boundary` lets it be one past a part's end, which an empty
    /// range names.
    isize at(isize x, size_field t, bool boundary = false)
    {
        if (moved.empty() || x < moved.front().old_start.*t)
            return x;
        for (auto const& p : moved)
        {
            auto const ends_here = boundary ? x <= p.old_end.*t : x < p.old_end.*t;
            if (x < p.old_start.*t || !ends_here)
                continue;
            auto const keeps = boundary ? x <= p.old_kept.*t : x < p.old_kept.*t;
            if (!keeps)
            {
                failed = true;
                return x;
            }
            return p.new_start.*t + (x - p.old_start.*t);
        }
        failed = true;
        return x;
    }

    void index(i32& x, size_field t)
    {
        if (x >= 0)
            x = i32(at(x, t));
    }

    void io(symbol_id& s)
    {
        if (is_valid(s))
            s = symbol_id(at(index_of(s), &table_sizes::symbols));
    }
    void io(type_id& t)
    {
        if (is_valid(t))
            t = type_id(at(index_of(t), &table_sizes::types));
    }

    template <class T>
    void io(ast::range_of<T>& r)
    {
        constexpr auto table = table_of_range<T>();
        if constexpr (table != nullptr)
            r.first = decltype(r.first)(at(isize(r.first), table, r.count == 0));
    }

    void io(cc::string&) {}
    void io(cc::string_view&) {}
    void io(builtins::registry const*&) {}

    template <class T>
    void io(cc::vector<T>& v)
    {
        if constexpr (!std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
            for (auto& e : v)
                io(e);
    }
    template <class T, isize N>
    void io(cc::fixed_array<T, N>& a)
    {
        for (auto& e : a)
            io(e);
    }
    template <class T>
    void io(cc::optional<T>& o)
    {
        if (o.has_value())
            io(o.value());
    }
    template <class T>
    void io(cc::shared_ptr<T>& p)
    {
        // what a layer shares holds no id of a link's own part, or it would not be shared
        (void)p;
    }
    template <class... Ts>
    void io(cc::variant<Ts...>& v)
    {
        v.visit([this](auto& alternative) { io(alternative); });
    }
    template <class K, class V>
    void io(cc::map<K, V>& m)
    {
        for (auto&& [key, value] : m)
            io(value);
    }
    template <class K>
    void io(cc::set<K>&)
    {
        // a set of the check pass is one of names
    }
    template <class T>
    void io(T*&)
    {
    }
    template <class T>
    void io(cc::span<T>&)
    {
    }

    // the raw positions of the check pass's records, which name an entry of a list by an integer
    void io(symbol& s)
    {
        walk(s);
        switch (s.kind)
        {
        case symbol_kind::function:
        case symbol_kind::test:
            index(s.info, &table_sizes::functions);
            break;
        case symbol_kind::binding:
            index(s.info, &table_sizes::bindings);
            break;
        case symbol_kind::pipeline:
            index(s.info, &table_sizes::pipelines);
            break;
        case symbol_kind::constant:
            index(s.info, &table_sizes::constants);
            break;
        case symbol_kind::sampler:
            index(s.info, &table_sizes::samplers);
            break;
        default:
            break;
        }
    }
    void io(member_info& m)
    {
        walk(m);
        index(m.static_sampler, &table_sizes::samplers);
    }
    void io(texel_store& t)
    {
        walk(t);
        index(t.store, &table_sizes::call_records);
        index(t.load, &table_sizes::call_records);
    }
    void io(file_tables& f)
    {
        walk(f);
        for (auto& c : f.call_of)
            index(c, &table_sizes::call_records);
    }
    void io(flat_call& c)
    {
        walk(c);
        index(c.text, &table_sizes::target_texts);
    }
    void io(wide_literal& w)
    {
        walk(w);
        index(w.instance, &table_sizes::function_instances);
    }
    void io(call_arguments& a)
    {
        walk(a);
        for (auto& l : a.literals)
            index(l, &table_sizes::literals);
        for (auto& f : a.functions)
            index(f, &table_sizes::function_arguments);
    }

    template <class T>
    void walk(T& v)
    {
        for_each_field(v, [this](auto& field) { io(field); });
    }

    template <class T>
    void io(T& v)
    {
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            (void)v;
        else if constexpr (std::is_aggregate_v<T>)
            walk(v);
        else
            static_assert(std::is_trivially_copyable_v<T>, "a type the relocation has no way to walk");
    }
};

template <class M>
struct map_types;
template <class K, class V>
struct map_types<cc::map<K, V>>
{
    using key = K;
    using value = V;
};
template <class K, class V>
struct map_types<layered_map<K, V>>
{
    using key = K;
    using value = V;
};

/// The fields of a checker in the order its own part is written and read: `out` first, which a relocated instance's
/// key reads, then the rest.
template <class C, class F>
void for_each_part_field(C& c, F&& f)
{
    f(c.out);
    for_each_field(c,
                   [&](auto& field)
                   {
                       // the names below a layer are what sealing it makes anew (`seal_layer`)
                       if constexpr (!std::is_same_v<std::remove_cvref_t<decltype(field)>, checked_module>)
                           if (static_cast<void const*>(&field) != static_cast<void const*>(&c.module_names_below))
                               f(field);
                   });
}

/// Writes what a link added to its base's state: its entries of every list past the base's, what it put in every map,
/// what it wrote into the notes of the base's functions, and its own value of everything else.
/// `part_reader` reads it back over another base, in the same order.
struct part_writer
{
    archive<true>& a;

    template <class F, class R>
    void member(F const& from_link, R const& link_root, R const& old_root)
    {
        // the registry is the process's
        if constexpr (!std::is_same_v<F, builtins::registry const> && !std::is_same_v<F, builtins::registry>)
            own(from_link, same_member(from_link, link_root, old_root));
    }

    template <class F>
    void own(F const& from_link, F const& from_old)
    {
        if constexpr (std::is_same_v<F, checked_module>)
            for_each_field(from_link, [&](auto const& inner) { member(inner, from_link, from_old); });
        else if constexpr (is_layered_v<F> || is_vector_v<F>)
        {
            auto from = from_old.size();
            if constexpr (is_layered_v<F>)
                if (from_link.shares_all_with(from_old))
                    from = from_link.size();
            (void)a.count(from_link.size() - from);
            for (auto i = from; i < from_link.size(); ++i)
            {
                auto e = std::remove_cvref_t<decltype(from_link[i])>(from_link[i]);
                a.io(e);
            }
            if constexpr (std::is_same_v<F, cc::vector<function_notes>>)
                writes_below(from_link, from_old);
        }
        else if constexpr (is_layered_map_v<F> || is_map_v<F>)
        {
            if constexpr (is_layered_map_v<F>)
                if (from_link.shares_all_with(from_old))
                {
                    (void)a.count(0);
                    return;
                }
            own_map(from_link, &from_old);
        }
        else if constexpr (is_set_v<F>)
        {
            auto own_keys = F();
            for (auto const& key : from_link)
                if (!from_old.contains(key))
                    (void)own_keys.insert(key);
            a.io(own_keys);
        }
        else if constexpr (is_shared_ptr_v<F>)
        {
            // what the link holds through the very pointer its base held, it never replaced: a fresh check keeps the
            // new base's
            auto is_base_s = from_link == from_old;
            a.raw(&is_base_s, sizeof(is_base_s));
            if (!is_base_s)
            {
                auto copy = from_link;
                a.io(copy);
            }
        }
        else
        {
            // a value of the check rather than a list of it
            auto copy = from_link;
            a.io(copy);
        }
    }

    /// Whether a map's entry `value` holds anything the old base's `before` does not.
    template <class V>
    bool is_own(V const& value, V const* before)
    {
        if constexpr (is_vector_v<V>)
            return (before != nullptr ? before->size() : isize(0)) != value.size();
        else if constexpr (is_map_v<V>)
            return before == nullptr || has_own(value, *before);
        else
            return before == nullptr;
    }

    template <class Map>
    bool has_own(Map const& link_map, Map const& old_map)
    {
        for (auto const& [key, value] : link_map)
            if (is_own(value, old_map.get_ptr(key)))
                return true;
        return false;
    }

    /// The entries of `link_map` the old base's map does not hold, in the order of their keys: a list's entries past
    /// the old one's, which are written behind how many it kept; a scope's own entries; any other value whole.
    template <class Map>
    void own_map(Map const& link_map, Map const* old_map)
    {
        using K = typename map_types<Map>::key;
        using V = typename map_types<Map>::value;
        // the link's own entries are few, so they are sorted after they are found
        auto entries = cc::vector<cc::pair<K const*, V const*>>();
        for (auto const& [key, value] : link_map)
            if (is_own(value, old_map != nullptr ? old_map->get_ptr(key) : nullptr))
                entries.push_back({&key, &value});
        cc::sort(entries, [](auto const& a, auto const& b) { return *a.first < *b.first; });
        (void)a.count(entries.size());
        for (auto const& [key, value] : entries)
        {
            auto k = *key;
            a.io(k);
            auto const* const before = old_map != nullptr ? old_map->get_ptr(*key) : nullptr;
            if constexpr (is_vector_v<V>)
            {
                auto kept = i64(before != nullptr ? before->size() : isize(0));
                a.raw(&kept, sizeof(kept));
                (void)a.count(value->size() - isize(kept));
                for (auto i = isize(kept); i < value->size(); ++i)
                {
                    auto e = (*value)[i];
                    a.io(e);
                }
            }
            else if constexpr (is_map_v<V>)
            {
                auto has_before = before != nullptr;
                a.raw(&has_before, sizeof(has_before));
                own_map(*value, before);
            }
            else
            {
                auto v = *value;
                a.io(v);
            }
        }
    }

    /// What the link wrote into an earlier function's notes, which the inlining caches on demand.
    void writes_below(cc::vector<function_notes> const& from_link, cc::vector<function_notes> const& from_old)
    {
        auto changed = cc::vector<isize>();
        for (auto i = isize(0); i < from_old.size(); ++i)
            if (cc::memcmp(&from_link[i], &from_old[i], sizeof(function_notes)) != 0)
                changed.push_back(i);
        (void)a.count(changed.size());
        for (auto const i : changed)
        {
            auto at = i64(i);
            a.raw(&at, sizeof(at));
            auto notes = from_link[i];
            a.raw(&notes, sizeof(notes));
        }
    }
};

/// Reads what `part_writer` wrote into a copy of a new base's state, every id moved through the parts that moved.
/// A table or a map the link added nothing to stays the very one the new base holds, shared.
struct part_reader
{
    archive<false>& a;
    relocator& r;
    checker& result;

    [[nodiscard]] bool failed() const { return a.failed || r.failed; }

    /// The key of a map relocated, where a key names an entry: a symbol, or a symbol and a direction.
    template <class K>
    K key_of(K key)
    {
        if constexpr (std::is_same_v<K, i32>)
            // `type_scopes` and `derivative_signatures`: a symbol's position
            return i32(r.at(key, &table_sizes::symbols));
        else if constexpr (std::is_same_v<K, i64>)
            // `derivative_overloads`: twice a symbol's position, and whether it is the reverse one
            return i64(r.at(key / 2, &table_sizes::symbols)) * 2 + key % 2;
        else
            return key;
    }

    template <class F>
    void member(F& field)
    {
        if constexpr (!std::is_same_v<F, builtins::registry const> && !std::is_same_v<F, builtins::registry>)
            own(field);
    }

    template <class F>
    void own(F& field)
    {
        if constexpr (std::is_same_v<F, checked_module>)
            for_each_field(field, [&](auto& inner) { member(inner); });
        else if constexpr (is_layered_v<F> || is_vector_v<F>)
        {
            auto const n = a.count(0);
            for (auto i = isize(0); i < n && !a.failed; ++i)
            {
                auto e = std::remove_cvref_t<decltype(field[0])>();
                a.io(e);
                r.io(e);
                field.push_back(cc::move(e));
            }
            if constexpr (is_layered_v<F>)
                if (n > 0)
                    field.freeze();
            if constexpr (std::is_same_v<F, cc::vector<function_notes>>)
                writes_below(field);
        }
        else if constexpr (is_layered_map_v<F> || is_map_v<F>)
        {
            auto const is_instances = static_cast<void const*>(&field) == static_cast<void const*>(&result.instances);
            auto const added = own_map(field, is_instances);
            if constexpr (is_layered_map_v<F>)
                if (added > 0)
                    field.freeze();
        }
        else if constexpr (is_set_v<F>)
        {
            auto keys = F();
            a.io(keys);
            for (auto const& key : keys)
                (void)field.insert(key);
        }
        else if constexpr (is_shared_ptr_v<F>)
        {
            auto is_base_s = false;
            a.raw(&is_base_s, sizeof(is_base_s));
            if (!is_base_s)
            {
                a.io(field);
                r.io(field);
            }
        }
        else
        {
            // a value of the check rather than a list of it: the link's, relocated
            a.io(field);
            r.io(field);
        }
    }

    /// The link's entries of a map into `into`, which holds the new base's; how many there were.
    /// A key the link added that the new base holds already, or a list under a key that the new base holds otherwise
    /// than the old one did, refuses: there a fresh check could have found the new base's entry instead.
    template <class Into>
    isize own_map(Into& into, bool is_instances)
    {
        using K = typename map_types<Into>::key;
        using V = typename map_types<Into>::value;
        auto const n = a.count(0);
        for (auto i = isize(0); i < n && !failed(); ++i)
        {
            auto key = K();
            a.io(key);
            auto const moved_key = key_of(key);
            auto const* const now = into.get_ptr(moved_key);
            if constexpr (is_vector_v<V>)
            {
                auto kept = i64(0);
                a.raw(&kept, sizeof(kept));
                if ((now != nullptr ? now->size() : isize(0)) != isize(kept))
                {
                    r.failed = true;
                    return n;
                }
                auto const m = a.count(0);
                auto& list = into[moved_key];
                for (auto j = isize(0); j < m && !a.failed; ++j)
                {
                    auto e = std::remove_cvref_t<decltype(list[0])>();
                    a.io(e);
                    r.io(e);
                    list.push_back(cc::move(e));
                }
            }
            else if constexpr (is_map_v<V>)
            {
                // a scope of names, which a later link may add to: an extension's method to an earlier type's
                auto has_before = false;
                a.raw(&has_before, sizeof(has_before));
                if (!has_before && now != nullptr)
                {
                    r.failed = true;
                    return n;
                }
                auto scope = now != nullptr ? *now : V();
                (void)own_map(scope, false);
                into[moved_key] = cc::move(scope);
            }
            else
            {
                auto value = V();
                a.io(value);
                r.io(value);
                auto at = moved_key;
                if constexpr (std::is_same_v<V, type_id>)
                    if (is_instances)
                    {
                        // an instance's key is a hash of its generic and its arguments, which moved with them
                        auto const& t = result.out.at(value);
                        at = K(instance_key_of(t.generic, result.out.at(t.arguments)));
                    }
                if (into.contains(at))
                {
                    r.failed = true;
                    return n;
                }
                into[at] = cc::move(value);
            }
        }
        return n;
    }

    /// The link's entries of an earlier function's notes, each where that function now stands.
    /// A changed link's functions are none of these, since a link that inlined one refuses (`bodies_read`).
    void writes_below(cc::vector<function_notes>& into)
    {
        auto const n = a.count(0);
        for (auto i = isize(0); i < n && !failed(); ++i)
        {
            auto from = i64(0);
            a.raw(&from, sizeof(from));
            auto notes = function_notes();
            a.raw(&notes, sizeof(notes));
            auto const at = r.at(isize(from), &table_sizes::functions);
            if (r.failed || at < 0 || at >= into.size())
            {
                r.failed = true;
                return;
            }
            into[at] = notes;
        }
    }

    [[nodiscard]] u64 instance_key_of(type_id generic, cc::span<type_id const> arguments) const
    {
        auto key = u64(0xcbf29ce484222325);
        auto const mix = [&](type_id t)
        {
            key ^= u64(u32(index_of(t)));
            key *= u64(0x100000001b3);
        };
        mix(generic);
        for (auto const a : arguments)
            mix(a);
        return key;
    }
};

template <class T>
void write_slice(archive<true>& a, layered<T> const& list, isize from, isize to)
{
    auto n = to - from;
    a.raw(&n, sizeof(n));
    for (auto i = from; i < to; ++i)
    {
        auto e = list[i];
        a.io(e);
    }
}

template <class T>
void write_slice(archive<true>& a, cc::vector<T> const& list, isize from, isize to)
{
    auto n = to - from;
    a.raw(&n, sizeof(n));
    for (auto i = from; i < to; ++i)
    {
        auto e = list[i];
        a.io(e);
    }
}
} // namespace

table_sizes sgl::check::impl::sizes_of(checker const& c)
{
    auto const& o = c.out;
    return {
        .symbols = o.symbols.size(),
        .types = o.types.size(),
        .members = o.members.size(),
        .enum_cases = o.enum_cases.size(),
        .functions = o.functions.size(),
        .parameters = o.parameters.size(),
        .bindings = o.bindings.size(),
        .binding_lists = o.binding_lists.size(),
        .type_lists = o.type_lists.size(),
        .samplers = o.samplers.size(),
        .pipelines = o.pipelines.size(),
        .pipeline_settings = o.pipeline_settings.size(),
        .constants = o.constants.size(),
        .call_records = o.call_records.size(),
        .written_arguments = o.written_arguments.size(),
        .call_slots = o.call_slots.size(),
        .function_instances = o.function_instances.size(),
        .target_texts = o.target_texts.size(),
        .literals = c.literals.size(),
        .function_arguments = c.function_arguments.size(),
    };
}

cc::hash128 sgl::check::impl::interface_hash(checker& c, table_sizes const& from, table_sizes const& to)
{
    auto bytes = cc::vector<byte>();
    auto a = archive<true>{.out = &bytes, .positionless = true};
    auto const& o = c.out;
    write_slice(a, o.symbols, from.symbols, to.symbols);
    write_slice(a, o.types, from.types, to.types);
    write_slice(a, o.members, from.members, to.members);
    write_slice(a, o.enum_cases, from.enum_cases, to.enum_cases);
    write_slice(a, o.functions, from.functions, to.functions);
    write_slice(a, o.parameters, from.parameters, to.parameters);
    write_slice(a, o.bindings, from.bindings, to.bindings);
    write_slice(a, o.binding_lists, from.binding_lists, to.binding_lists);
    write_slice(a, o.type_lists, from.type_lists, to.type_lists);
    write_slice(a, o.samplers, from.samplers, to.samplers);
    write_slice(a, o.pipelines, from.pipelines, to.pipelines);
    write_slice(a, o.pipeline_settings, from.pipeline_settings, to.pipeline_settings);
    write_slice(a, o.constants, from.constants, to.constants);
    write_slice(a, o.call_records, from.call_records, to.call_records);
    write_slice(a, o.written_arguments, from.written_arguments, to.written_arguments);
    write_slice(a, o.call_slots, from.call_slots, to.call_slots);
    write_slice(a, o.function_instances, from.function_instances, to.function_instances);
    write_slice(a, o.target_texts, from.target_texts, to.target_texts);
    write_slice(a, c.literals, from.literals, to.literals);
    write_slice(a, c.function_arguments, from.function_arguments, to.function_arguments);
    return cc::hash128::create(bytes, u64(bytes.size()));
}

void sgl::check::impl::write_own_part(archive<true>& a, checker const& link, checker const& old_base)
{
    auto w = part_writer{.a = a};
    for_each_part_field(link, [&](auto const& field) { w.member(field, link, old_base); });
}

bool sgl::check::impl::read_own_part(archive<false>& a, checker& state, cc::span<moved_part const> moved)
{
    auto r = relocator{.moved = moved};
    auto reader = part_reader{.a = a, .r = r, .result = state};
    for_each_part_field(state, [&](auto& field) { reader.member(field); });
    return !reader.failed() && a.at == a.in.size();
}

table_sizes sgl::check::impl::plus(table_sizes a, table_sizes const& from, table_sizes const& to)
{
    for (auto const t : k_tables)
        a.*t += to.*t - from.*t;
    return a;
}

bool sgl::check::impl::same_shape(table_sizes const& a_from,
                                  table_sizes const& a_to,
                                  table_sizes const& b_from,
                                  table_sizes const& b_to)
{
    for (auto const t : k_tables)
        if (a_to.*t - a_from.*t != b_to.*t - b_from.*t)
            return false;
    return true;
}
