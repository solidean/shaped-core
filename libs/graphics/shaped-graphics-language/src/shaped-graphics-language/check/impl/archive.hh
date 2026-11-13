#pragma once

#include <clean-core/algorithm/sort.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/map.hh>
#include <clean-core/container/set.hh>
#include <clean-core/container/variant.hh>
#include <clean-core/memory/shared_ptr.hh>
#include <clean-core/memory/unique_ptr.hh>
#include <shaped-graphics-language/check/impl/checker.hh>
#include <shaped-graphics-language/check/impl/fields.hh>

#include <type_traits>

/// The walk over a checker's whole state that an artifact writes and reads back, and that the interface of a layer of
/// a chain is hashed by (relocation, `relocate.hh`).
namespace sgl::check::impl
{
/// An id that names a place in a file's parse or AST, which an edit elsewhere in the file moves.
template <class T>
inline constexpr bool is_position_id_v
    = std::is_same_v<T, ast::expr_id> || std::is_same_v<T, ast::stmt_id> || std::is_same_v<T, ast::decl_id>
   || std::is_same_v<T, ast::field_id> || std::is_same_v<T, line_id> || std::is_same_v<T, token_id>
   || std::is_same_v<T, group_id> || std::is_same_v<T, form_id>;

/// An element of a list of the check pass, which a range of it names by position there rather than in a file's AST.
template <class T>
inline constexpr bool is_checked_element_v
    = std::is_same_v<T, type_id> || std::is_same_v<T, symbol_id> || std::is_same_v<T, member_info>
   || std::is_same_v<T, enum_case_info> || std::is_same_v<T, parameter> || std::is_same_v<T, pipeline_setting>
   || std::is_same_v<T, written_argument> || std::is_same_v<T, i32> || std::is_same_v<T, flat_expr_id>
   || std::is_same_v<T, flat_stmt_id> || std::is_same_v<T, flat_arm> || std::is_same_v<T, call_site>
   || std::is_same_v<T, flat_check_node>;

template <class T>
inline constexpr bool is_vector_v = false;
template <class T>
inline constexpr bool is_vector_v<cc::vector<T>> = true;

template <class T>
inline constexpr bool is_fixed_array_v = false;
template <class T, isize N>
inline constexpr bool is_fixed_array_v<cc::fixed_array<T, N>> = true;

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

template <class T>
inline constexpr bool is_shared_ptr_v = false;
template <class T>
inline constexpr bool is_shared_ptr_v<cc::shared_ptr<T>> = true;

/// The member of `other` at the place `field` has in `root`, both of one type.
template <class F, class R>
F const& same_member(F const& field, R const& root, R const& other)
{
    auto const offset = reinterpret_cast<char const*>(&field) - reinterpret_cast<char const*>(&root);
    return *reinterpret_cast<F const*>(reinterpret_cast<char const*>(&other) + offset);
}

/// The entries of a map in the order of their keys, so a layer is the same bytes whatever order its maps were filled in.
template <class Key, class Value, class Map>
cc::vector<cc::pair<Key const*, Value const*>> sorted_entries(Map const& m)
{
    auto out = cc::vector<cc::pair<Key const*, Value const*>>();
    for (auto const& [key, value] : m)
        out.push_back({&key, &value});
    cc::sort(out, [](auto const& a, auto const& b) { return *a.first < *b.first; });
    return out;
}

/// One pass over a checked layer, writing it or reading it back: the same walk both ways, so the two cannot disagree.
///
/// A field is written by its type: a number or an enum as its bytes, a container as its size and then its elements,
/// and an aggregate as each of its fields in turn (`for_each_field`).
/// A string view is the one value that names memory, so it is written as a place in one of the files' sources where it
/// is one, and as its own text otherwise, which a read keeps in `texts`.
/// A pointer and a span are a layer's only when they are empty, and the registry is the caller's, so neither is written.
template <bool Writing>
struct archive
{
    /// Writing: where the bytes go.
    cc::vector<byte>* out = nullptr;
    /// Reading: the bytes, and how far the walk got.
    cc::span<byte const> in;
    isize at = 0;
    /// The sources of the layer's files, which a string view of the layer points into.
    cc::span<cc::string_view const> sources;
    /// The sources of other layers' files, which an edit may have moved: a view into one is written as its place and its
    /// text, and read back as that place where the text there is still the same.
    cc::span<cc::string_view const> elsewhere;
    /// Reading: where a string view in none of `sources` keeps its text.
    cc::vector<cc::unique_ptr<cc::string>>* texts = nullptr;
    builtins::registry const* registry = nullptr;
    /// Writing: what the layer holds that no artifact carries; reading: the bytes ran out or held nonsense.
    bool failed = false;
    /// Writing only: every place in a file written as nothing, and every string view as its text, so two layers that
    /// differ in where their declarations stand alone are the same bytes; what a layer's interface is hashed by.
    bool positionless = false;
    /// Writing only: every string view as its text, so two layers whose views point into two copies of one text are the
    /// same bytes; what a test compares a moved layer with a checked one by.
    bool views_as_text = false;

    void raw(void* data, isize size)
    {
        if constexpr (Writing)
            out->push_back_range(cc::span<byte const>(static_cast<byte const*>(data), size));
        else
        {
            if (failed || at + size > in.size())
            {
                failed = true;
                cc::memset(data, 0, size);
                return;
            }
            cc::memcpy(data, in.data() + at, size);
            at += size;
        }
    }

    /// The size of a container: written as is, and read only as far as the bytes left could hold.
    isize count(isize n)
    {
        auto v = i64(n);
        raw(&v, sizeof(v));
        if constexpr (!Writing)
            if (v < 0 || v > in.size() - at)
            {
                failed = true;
                return 0;
            }
        return isize(v);
    }

    void io(cc::string& s)
    {
        auto const n = count(s.size());
        if constexpr (Writing)
            raw(const_cast<char*>(s.data()), n);
        else
        {
            s = cc::string::create_uninitialized(n);
            raw(s.data(), n);
        }
    }

    void io(cc::string_view& v)
    {
        // 0: empty; 1: a place in a source; 2: its own text; 3: a place elsewhere, and its text
        auto tag = u8(0);
        auto source = i32(-1);
        auto offset = i64(0);
        auto size = i64(v.size());
        if constexpr (Writing)
        {
            auto const find = [&](cc::span<cc::string_view const> in, u8 found)
            {
                for (auto i = isize(0); i < in.size() && tag == 2; ++i)
                    if (v.data() >= in[i].data() && v.data() + v.size() <= in[i].data() + in[i].size())
                    {
                        tag = found;
                        source = i32(i);
                        offset = v.data() - in[i].data();
                    }
            };
            if (!v.empty())
            {
                tag = 2;
                if (!positionless && !views_as_text)
                {
                    find(sources, 1);
                    find(elsewhere, 3);
                }
            }
        }
        raw(&tag, sizeof(tag));
        if (tag == 0)
        {
            if constexpr (!Writing)
                v = {};
            return;
        }
        if (tag == 1)
        {
            raw(&source, sizeof(source));
            raw(&offset, sizeof(offset));
            raw(&size, sizeof(size));
            if constexpr (!Writing)
            {
                if (failed || source < 0 || source >= sources.size() || offset < 0 || size < 0
                    || offset + size > sources[source].size())
                {
                    failed = true;
                    v = {};
                    return;
                }
                v = sources[source].subview({.offset = offset, .size = size});
            }
            return;
        }
        if (tag == 3)
        {
            raw(&source, sizeof(source));
            raw(&offset, sizeof(offset));
        }
        if constexpr (Writing)
        {
            auto text = cc::string(v);
            io(text);
        }
        else
        {
            auto text = cc::make_unique<cc::string>();
            io(*text);
            if (tag == 3 && !failed && source >= 0 && source < elsewhere.size() && offset >= 0
                && offset + text->size() <= elsewhere[source].size()
                && elsewhere[source].subview({.offset = offset, .size = text->size()}) == cc::string_view(*text))
                v = elsewhere[source].subview({.offset = offset, .size = text->size()});
            else
            {
                v = cc::string_view(*text);
                texts->push_back(cc::move(text));
            }
        }
        if (tag > 3)
            failed = true;
    }

    template <class T>
    void io(cc::vector<T>& v)
    {
        auto const n = count(v.size());
        if constexpr (!Writing)
        {
            v.clear();
            v.resize_to_defaulted(n);
        }
        if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            raw(v.data(), n * isize(sizeof(T)));
        else
            for (auto i = isize(0); i < n; ++i)
                io(v[i]);
    }

    template <class T>
    void io(layered<T>& v)
    {
        auto const n = count(v.size());
        if constexpr (Writing)
            for (auto i = isize(0); i < n; ++i)
                io(const_cast<T&>(v[i]));
        else
        {
            v = {};
            for (auto i = isize(0); i < n && !failed; ++i)
            {
                auto e = T();
                io(e);
                v.push_back(cc::move(e));
            }
            v.freeze();
        }
    }

    template <class K, class V>
    void io(cc::map<K, V>& m)
    {
        auto const n = count(m.size());
        if constexpr (Writing)
            for (auto const& [key, value] : sorted_entries<K, V>(m))
            {
                io(const_cast<K&>(*key));
                io(const_cast<V&>(*value));
            }
        else
        {
            m.clear();
            for (auto i = isize(0); i < n && !failed; ++i)
            {
                auto key = K();
                auto value = V();
                io(key);
                io(value);
                m[key] = cc::move(value);
            }
        }
    }

    template <class K>
    void io(cc::set<K>& s)
    {
        auto const n = count(s.size());
        if constexpr (Writing)
        {
            auto keys = cc::vector<K const*>();
            for (auto const& key : s)
                keys.push_back(&key);
            cc::sort(keys, [](K const* a, K const* b) { return *a < *b; });
            for (auto const* key : keys)
                io(const_cast<K&>(*key));
        }
        else
        {
            s.clear();
            for (auto i = isize(0); i < n && !failed; ++i)
            {
                auto key = K();
                io(key);
                (void)s.insert(cc::move(key));
            }
        }
    }

    template <class K, class V>
    void io(layered_map<K, V>& m)
    {
        auto n = isize(0);
        if constexpr (Writing)
            for (auto const& entry : m)
                (void)entry, ++n;
        n = count(n);
        if constexpr (Writing)
            for (auto const& [key, value] : sorted_entries<K, V>(m))
            {
                io(const_cast<K&>(*key));
                io(const_cast<V&>(*value));
            }
        else
        {
            m = {};
            for (auto i = isize(0); i < n && !failed; ++i)
            {
                auto key = K();
                auto value = V();
                io(key);
                io(value);
                m[key] = cc::move(value);
            }
            m.freeze();
        }
    }

    template <class T>
    void io(cc::optional<T>& o)
    {
        auto has = o.has_value();
        raw(&has, sizeof(has));
        if constexpr (Writing)
        {
            if (has)
                io(o.value());
        }
        else if (has)
        {
            auto value = T();
            io(value);
            o = cc::move(value);
        }
        else
            o = {};
    }

    template <class T>
    void io(cc::shared_ptr<T>& p)
    {
        using value_type = std::remove_const_t<T>;
        auto has = p != nullptr;
        raw(&has, sizeof(has));
        if constexpr (Writing)
        {
            if (has)
                io(const_cast<value_type&>(*p));
        }
        else if (has)
        {
            auto value = value_type();
            io(value);
            p = cc::make_shared<value_type>(cc::move(value));
        }
        else
            p = nullptr;
    }

    template <class T, isize N>
    void io(cc::fixed_array<T, N>& a)
    {
        for (auto i = isize(0); i < N; ++i)
            io(a[i]);
    }

    /// The caller's views, a layer's only while it is empty: the files, the options.
    template <class T>
    void io(cc::span<T>& s)
    {
        if constexpr (Writing)
            failed = failed || !s.empty();
        else
            s = {};
    }

    /// The registry, which a load takes from its caller.
    void io(builtins::registry const*& p)
    {
        if constexpr (!Writing)
            p = registry;
    }
    void io(builtins::registry const&) {}

    /// Any other pointer is a layer's only while it is null.
    template <class T>
    void io(T*& p)
    {
        if constexpr (Writing)
            failed = failed || p != nullptr;
        else
            p = nullptr;
    }

    /// Its alternative's position, then the alternative.
    template <class... Ts>
    void io(cc::variant<Ts...>& v)
    {
        auto index = i32(v.index());
        raw(&index, sizeof(index));
        if constexpr (!Writing)
        {
            if (failed || index < 0 || index >= i32(sizeof...(Ts)))
            {
                failed = true;
                return;
            }
            emplace_at<0>(v, index);
        }
        v.visit([this](auto& alternative) { io(alternative); });
    }

    template <isize I, class... Ts>
    static void emplace_at(cc::variant<Ts...>& v, i32 index)
    {
        if constexpr (I < isize(sizeof...(Ts)))
        {
            if (index == I)
                (void)v.template emplace<I>();
            else
                emplace_at<I + 1>(v, index);
        }
    }

    /// A flat tree's mint names what it took and what its own mint is, and none of what it points at.
    /// Those are the names other mints took, which only an emitter going on from it reads, and a tree a layer holds is
    /// a test's, which the interpreter runs and nothing emits; a mint read back points at nothing.
    void io(name_mint& m)
    {
        io(m.taken);
        io(m.index);
        io(m.next_suffix);
        if constexpr (!Writing)
        {
            m.parent = nullptr;
            m.also_taken = nullptr;
            m.also_taken_below = nullptr;
            m.also_reserved = nullptr;
        }
    }

    /// What a body adds to a function's entry once its interface is checked: the features it uses, a task stage's
    /// payload, a pixel stage's targets, a test's bindings; a user of the function reads them only by inlining it.
    void io(function_info& f)
    {
        if (!positionless)
            return for_each_field(f, [this](auto& field) { io(field); });
        auto copy = f;
        copy.features = {};
        copy.mesh_payload = type_id::none;
        copy.pixel_targets = type_id::none;
        copy.bindings = {};
        for_each_field(copy, [this](auto& field) { io(field); });
    }

    void io(source_span& s)
    {
        if (positionless)
        {
            auto none = source_span{};
            raw(&none, sizeof(none));
        }
        else
            raw(&s, sizeof(s));
    }

    template <class T>
    void io(ast::range_of<T>& r)
    {
        if constexpr (!is_checked_element_v<T>)
            if (positionless)
            {
                auto none = ast::range_of<T>{};
                for_each_field(none, [this](auto& field) { io(field); });
                return;
            }
        for_each_field(r, [this](auto& field) { io(field); });
    }

    template <class T>
    void io(T& v)
    {
        if constexpr (is_position_id_v<T>)
        {
            if (positionless)
            {
                auto none = T();
                raw(&none, sizeof(T));
            }
            else
                raw(&v, sizeof(T));
        }
        else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            raw(&v, sizeof(T));
        else if constexpr (std::is_aggregate_v<T>)
            for_each_field(v, [this](auto& field) { io(field); });
        else
        {
            static_assert(std::is_trivially_copyable_v<T>, "a type the artifact has no way to write");
            raw(&v, sizeof(T));
        }
    }
};

} // namespace sgl::check::impl
