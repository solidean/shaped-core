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

template <class T>
inline constexpr bool is_vector_v = false;
template <class T>
inline constexpr bool is_vector_v<cc::vector<T>> = true;

template <class T>
inline constexpr bool is_layered_v = false;
template <class T>
inline constexpr bool is_layered_v<layered<T>> = true;

template <class T>
inline constexpr bool is_layered_map_v = false;
template <class K, class V>
inline constexpr bool is_layered_map_v<layered_map<K, V>> = true;

template <class T>
inline constexpr bool is_map_v = false;
template <class K, class V>
inline constexpr bool is_map_v<cc::map<K, V>> = true;

template <class T>
inline constexpr bool is_set_v = false;
template <class K>
inline constexpr bool is_set_v<cc::set<K>> = true;

/// The member of `other` at the place `field` has in `root`, both of one type.
template <class F, class R>
F const& same_member(F const& field, R const& root, R const& other)
{
    auto const offset = reinterpret_cast<char const*>(&field) - reinterpret_cast<char const*>(&root);
    return *reinterpret_cast<F const*>(reinterpret_cast<char const*>(&other) + offset);
}

/// Adds to a copy of the new base's state what the link added past the old base's, relocated: its entries of every
/// list, what it put in every map, and its own value of everything else.
/// A table or a map the link added nothing to is the very one the new base holds, shared.
struct splicer
{
    relocator& r;
    checker& result;

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

    /// The link's entries past the old base's, relocated, behind `into`'s.
    template <class List, class Into>
    void append_own(Into& into, List const& link_list, List const& old_list)
    {
        for (auto i = old_list.size(); i < link_list.size(); ++i)
        {
            auto e = std::remove_cvref_t<decltype(link_list[i])>(link_list[i]);
            r.io(e);
            into.push_back(cc::move(e));
        }
    }

    /// What the link put in a map past what the old base holds, relocated, into `into`, which holds the new base's.
    /// A key the link added that the new base holds and the old one did not, or a list under a key both hold that
    /// the new base holds otherwise, refuses: there a fresh check could have found the new base's entry instead.
    template <class Into, class Map>
    void merge_own(Into& into, Map const& link_map, Map const& old_map, bool is_instances)
    {
        for (auto const& [key, value] : link_map)
        {
            if (r.failed)
                return;
            using V = std::remove_cvref_t<decltype(value)>;
            auto const* const before = old_map.get_ptr(key);
            if constexpr (is_vector_v<V>)
            {
                auto const kept = before != nullptr ? before->size() : isize(0);
                if (kept == value.size())
                    continue;
                auto const* const now = into.get_ptr(key);
                if ((now != nullptr ? now->size() : isize(0)) != kept)
                {
                    r.failed = true;
                    return;
                }
                auto& list = into[key_of(key)];
                for (auto i = kept; i < value.size(); ++i)
                {
                    auto e = value[i];
                    r.io(e);
                    list.push_back(cc::move(e));
                }
            }
            else if constexpr (is_map_v<V>)
            {
                // a scope of names, which a later link may add to: an extension's method to an earlier type's
                auto const* const now = into.get_ptr(key);
                if (before == nullptr && now != nullptr)
                {
                    r.failed = true;
                    return;
                }
                auto scope = now != nullptr ? *now : V();
                merge_own(scope, value, before != nullptr ? *before : V(), false);
                into[key_of(key)] = cc::move(scope);
            }
            else
            {
                if (before != nullptr)
                    continue;
                auto moved_value = value;
                r.io(moved_value);
                if constexpr (std::is_same_v<V, type_id>)
                    if (is_instances)
                    {
                        // an instance's key is a hash of its generic and its arguments, which moved with them
                        auto const& t = result.out.at(moved_value);
                        auto const moved_key = instance_key_of(t.generic, result.out.at(t.arguments));
                        if (into.contains(moved_key))
                        {
                            r.failed = true;
                            return;
                        }
                        into[moved_key] = moved_value;
                        continue;
                    }
                auto const moved_key = key_of(key);
                if (into.contains(moved_key))
                {
                    r.failed = true;
                    return;
                }
                into[moved_key] = cc::move(moved_value);
            }
        }
    }

    /// `field` of the result, which holds the new base's value, given the link's and the old base's.
    template <class F, class R>
    void member(F& field, R const& link_root, R const& old_root, R& result_root)
    {
        // the registry is the process's
        if constexpr (std::is_same_v<F, builtins::registry const> || std::is_same_v<F, builtins::registry>)
            return;
        else
            own_member(field, same_member(field, result_root, link_root), same_member(field, result_root, old_root));
    }

    template <class F>
    void own_member(F& field, F const& from_link, F const& from_old)
    {
        if constexpr (std::is_same_v<F, checked_module>)
            for_each_field(field, [&](auto& inner) { member(inner, from_link, from_old, field); });
        else if constexpr (is_layered_v<F>)
        {
            if (from_link.shares_all_with(from_old))
                return;
            append_own(field, from_link, from_old);
            field.freeze();
        }
        else if constexpr (is_vector_v<F>)
        {
            append_own(field, from_link, from_old);
            if constexpr (std::is_same_v<F, cc::vector<function_notes>>)
                own_writes_below(field, from_link, from_old);
        }
        else if constexpr (is_layered_map_v<F>)
        {
            if (from_link.shares_all_with(from_old))
                return;
            auto const is_instances = static_cast<void const*>(&field) == static_cast<void const*>(&result.instances);
            merge_own(field, from_link, from_old, is_instances);
            field.freeze();
        }
        else if constexpr (is_map_v<F>)
            merge_own(field, from_link, from_old, false);
        else if constexpr (is_set_v<F>)
        {
            for (auto const& key : from_link)
                (void)field.insert(key);
        }
        else
        {
            // a value of the check rather than a list of it: the link's, relocated
            field = from_link;
            r.io(field);
        }
    }

    /// What the link wrote into an earlier function's notes, which the inlining caches on demand: where its entry
    /// differs from the old base's, the link's stands at that function's new place.
    /// A changed link's functions are none of these, since a link that inlined one refuses (`bodies_read`).
    void own_writes_below(cc::vector<function_notes>& into,
                          cc::vector<function_notes> const& from_link,
                          cc::vector<function_notes> const& from_old)
    {
        for (auto i = isize(0); i < from_old.size(); ++i)
        {
            if (cc::memcmp(&from_link[i], &from_old[i], sizeof(function_notes)) == 0)
                continue;
            auto const was_failed = r.failed;
            auto const at = r.at(i, &table_sizes::functions);
            if (r.failed && !was_failed)
                return;
            into[at] = from_link[i];
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

void checker::note_body_read(symbol_id function) const
{
    if (!records_interface || !is_valid(function))
        return;
    auto const file = out.at(function).file;
    if (is_prelude_file(file) || file >= run_from_files)
        return;
    for (auto i = bodies_read_from; i < bodies_read.size(); ++i)
        if (bodies_read[i] == function)
            return;
    bodies_read.push_back(function);
}

cc::unique_ptr<checker> sgl::check::impl::relocated_state(checker const& link,
                                                          checker const& old_base,
                                                          checker const& new_base,
                                                          cc::span<moved_part const> moved)
{
    // the link's own part moves as a whole, behind the new base
    auto parts = cc::vector<moved_part>::create_copy_of(moved);
    parts.push_back({
        .old_start = sizes_of(old_base),
        .old_kept = sizes_of(link),
        .old_end = sizes_of(link),
        .new_start = sizes_of(new_base),
    });
    auto r = relocator{.moved = parts};
    auto result = cc::make_unique<checker>(new_base);
    auto s = splicer{.r = r, .result = *result};
    // `out` first, which a relocated instance's key reads
    s.member(result->out, link, old_base, *result);
    for_each_field(*result,
                   [&](auto& field)
                   {
                       if constexpr (!std::is_same_v<std::remove_cvref_t<decltype(field)>, checked_module>)
                           s.member(field, link, old_base, *result);
                   });
    if (r.failed)
        return nullptr;
    return result;
}

namespace
{
table_sizes plus(table_sizes a, table_sizes const& from, table_sizes const& to)
{
    for (auto const t : k_tables)
        a.*t += to.*t - from.*t;
    return a;
}

bool same_shape(table_sizes const& a_from, table_sizes const& a_to, table_sizes const& b_from, table_sizes const& b_to)
{
    for (auto const t : k_tables)
        if (a_to.*t - a_from.*t != b_to.*t - b_from.*t)
            return false;
    return true;
}
} // namespace

cc::optional<checked_prelude> sgl::check::relocate_link(checked_prelude const& link, cc::span<moved_link const> moved)
{
    CC_ASSERT(!moved.empty(), "a link is relocated onto the links an edit moved");
    auto const& old_base = *moved.back().before;
    auto const& new_base = *moved.back().after;
    CC_ASSERT(link._is_library && old_base._is_library && new_base._is_library, "relocation moves links of a chain");

    // Each changed link's part: a relocated one moved whole, one checked again keeps its interface where it hashes the
    // same, and nothing where it does not.
    auto parts = cc::vector<impl::moved_part>();
    for (auto const& m : moved)
    {
        if (m.before == m.after)
            continue;
        auto const& b = *m.before->_state;
        auto const& a = *m.after->_state;
        auto part = impl::moved_part{.old_start = b.run_start, .old_end = impl::sizes_of(b), .new_start = a.run_start};
        // a link checked again into what it was, but where it stands, keeps every entry
        if (m.after->_is_relocated
            || (b.part_digest == a.part_digest
                && same_shape(b.run_start, impl::sizes_of(b), a.run_start, impl::sizes_of(a))))
            part.old_kept = part.old_end;
        else if (b.interface_digest == a.interface_digest
                 && same_shape(b.run_start, b.interface_end, a.run_start, a.interface_end))
            part.old_kept = b.interface_end;
        else
            part.old_kept = b.run_start;
        parts.push_back(part);
    }

    // a body of a link checked again is what the link inlined or instantiated, and would read otherwise now
    auto const& own = *link._state;
    for (auto i = old_base._state->bodies_read.size(); i < own.bodies_read.size(); ++i)
        for (auto const& m : moved)
            if (m.before != m.after && !m.after->_is_relocated)
            {
                auto const& b = *m.before->_state;
                auto const at = index_of(own.bodies_read[i]);
                if (at >= b.run_start.symbols && at < impl::sizes_of(b).symbols)
                    return {};
            }

    auto state = impl::relocated_state(own, *old_base._state, *new_base._state, parts);
    if (state == nullptr)
        return {};
    // the link's own part starts where the new base ends, its interface as long as before
    state->run_start = impl::sizes_of(*new_base._state);
    state->interface_end = plus(state->run_start, own.run_start, own.interface_end);
    // what the interface hashes: its entries as they now stand, which a body checked after it never writes but where it
    // moved (`archive::io(function_info&)`)
    state->interface_digest = impl::interface_hash(*state, state->run_start, state->interface_end);
    state->part_digest = impl::interface_hash(*state, state->run_start, impl::sizes_of(*state));
    state->files = {};

    auto result = checked_prelude();
    result._files.push_back_range(cc::span<module_file const>(new_base._files));
    for (auto i = old_base._files.size(); i < link._files.size(); ++i)
        result._files.push_back(link._files[i]);
    result._prelude_count = link._prelude_count;
    result._library_order = link._library_order;
    result._modules = link._modules;
    result._is_library = true;
    result._options = link._options;
    result._is_relocated = true;
    result._names_by_reach = cc::make_unique<cc::mutex<cc::map<cc::string, cc::shared_ptr<cc::set<cc::string>>>>>();
    result._resume = impl::seal_layer(*state, i32(result._files.size()));
    result._state = cc::move(state);
    return result;
}

namespace
{
/// Finds where two values of one type first differ, as written by the artifact's walk, and names the path there.
struct differ
{
    cc::span<cc::string_view const> sources;
    cc::string path;

    template <class T>
    cc::vector<byte> bytes_of(T const& v)
    {
        auto out = cc::vector<byte>();
        auto a = archive<true>{.out = &out, .sources = sources};
        a.io(const_cast<T&>(v));
        return out;
    }

    template <class T>
    bool same(T const& a, T const& b)
    {
        auto const x = bytes_of(a);
        auto const y = bytes_of(b);
        if (x.size() != y.size())
            return false;
        for (auto i = isize(0); i < x.size(); ++i)
            if (x[i] != y[i])
                return false;
        return true;
    }

    /// Whether `a` and `b` differ, `path` naming the first place where.
    template <class T>
    bool differs(T const& a, T const& b)
    {
        if constexpr (std::is_same_v<T, builtins::registry const> || std::is_same_v<T, builtins::registry>)
            return false;
        else
        {
            if (same(a, b))
                return false;
            if constexpr (is_layered_v<T> || is_vector_v<T>)
            {
                if (a.size() != b.size())
                {
                    path.appendf(" size {} against {}", a.size(), b.size());
                    return true;
                }
                for (auto i = isize(0); i < a.size(); ++i)
                    if (path.appendf("[{}]", i); differs(a[i], b[i]))
                        return true;
                    else
                        path.resize_down_to(path.rfind('['));
                return true;
            }
            else if constexpr (std::is_aggregate_v<T> && !std::is_array_v<T>)
            {
                auto index = 0;
                auto found = false;
                for_each_field(a,
                               [&](auto const& field)
                               {
                                   if (found)
                                       return;
                                   auto const& other = same_member(field, a, b);
                                   auto const mark = path.size();
                                   path.appendf(".{}", index++);
                                   if (differs(field, other))
                                       found = true;
                                   else
                                       path.resize_down_to(mark);
                               });
                return true;
            }
            else
            {
                if constexpr (std::is_arithmetic_v<T>)
                    path.appendf(" = {} against {}", a, b);
                else if constexpr (std::is_enum_v<T>)
                    path.appendf(" = {} against {}", i64(a), i64(b));
                return true;
            }
        }
    }
};
} // namespace

cc::string sgl::check::layer_difference(checked_prelude const& a, checked_prelude const& b)
{
    auto sources = cc::vector<cc::string_view>();
    for (auto const& f : a._files)
        sources.push_back(cc::string_view(f.file.source));
    auto d = differ{.sources = sources};
    if (!d.differs(*a._state, *b._state))
        return {};
    return cc::move(d.path);
}
