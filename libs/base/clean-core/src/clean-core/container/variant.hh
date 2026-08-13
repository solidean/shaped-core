#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh>
#include <clean-core/common/impl/small_size_type.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>

#include <type_traits>
#include <utility>

namespace cc::impl
{
template <class... Ts>
union variant_storage;

/// Terminator of the recursion - no alternatives, nothing to store.
template <>
union variant_storage<>
{
};

/// A recursive union, because an anonymous union cannot be generated from a pack.
/// Nothing here is ever active on its own: cc::variant places the alternative it selected with placement new.
template <class T, class... Ts>
union variant_storage<T, Ts...> // NOLINT(cppcoreguidelines-special-member-functions)
{
    /// Activates no member at all - the union starts out holding nothing.
    constexpr variant_storage() {}

    /// Defaulted when every alternative is trivially destructible, empty otherwise.
    /// cc::variant runs the active alternative's destructor itself, since only it knows which one that is.
    ~variant_storage()
        requires(std::is_trivially_destructible_v<T> && std::is_trivially_destructible_v<variant_storage<Ts...>>)
    = default;
    ~variant_storage()
        requires(!std::is_trivially_destructible_v<T> || !std::is_trivially_destructible_v<variant_storage<Ts...>>)
    {
    }

    T _head;
    variant_storage<Ts...> _tail;
};

/// Returns the I-th alternative of a variant_storage, forwarding the storage's value category.
/// The alternative must be the active one - this only walks the union, it does not check anything.
template <std::size_t I, class StorageT>
[[nodiscard]] constexpr auto&& variant_alternative(StorageT&& storage) noexcept
{
    if constexpr (I == 0)
        return static_cast<StorageT&&>(storage)._head;
    else
        return cc::impl::variant_alternative<I - 1>(static_cast<StorageT&&>(storage)._tail);
}

template <std::size_t I, class... Ts>
using variant_alternative_t
    = std::remove_reference_t<decltype(cc::impl::variant_alternative<I>(std::declval<variant_storage<Ts...>&>()))>;

/// Turns the runtime index i into a compile-time one and calls f(std::integral_constant<std::size_t, I>{}).
/// The single dispatch primitive: visit, the copy/move members, the destructor, == and hash all run through it.
/// An if-chain rather than a table of function pointers, so it inlines and stays usable in a constant expression.
template <std::size_t N, std::size_t I = 0, class F>
constexpr decltype(auto) variant_dispatch(std::size_t i, F&& f)
{
    static_assert(N > 0, "variant_dispatch needs at least one alternative");

    if constexpr (I + 1 == N)
    {
        CC_ASSERT(i == I, "variant index out of range");
        return cc::forward<F>(f)(std::integral_constant<std::size_t, I>{});
    }
    else if (i == I)
        return cc::forward<F>(f)(std::integral_constant<std::size_t, I>{});
    else
        return cc::impl::variant_dispatch<N, I + 1>(i, cc::forward<F>(f));
}

/// How many of Ts are exactly U, and the index of the first one.
template <class U, class... Ts>
inline constexpr std::size_t variant_exact_count = (std::size_t(std::is_same_v<U, Ts>) + ... + 0);

template <class U, class... Ts>
[[nodiscard]] constexpr std::size_t variant_exact_index()
{
    bool const matches[] = {std::is_same_v<U, Ts>...};
    for (std::size_t i = 0; i < sizeof...(Ts); ++i)
        if (matches[i])
            return i;
    return sizeof...(Ts);
}

/// The callable a visit runs: a lone handler passes through untouched, so plain function pointers work too,
/// while several handlers are combined into one overload set.
template <class... Fs>
[[nodiscard]] constexpr decltype(auto) variant_make_visitor(Fs&&... fs)
{
    if constexpr (sizeof...(Fs) == 1)
        return (cc::forward<Fs>(fs), ...);
    else
        return cc::overloaded{cc::forward<Fs>(fs)...};
}

template <std::size_t I>
struct variant_index_tag
{
};
} // namespace cc::impl

/// Sum type holding exactly one of the alternatives Ts, discriminated by index.
/// There is NO valueless state: index() is always in [0, alternative_count).
/// The alternatives must be PAIRWISE DISTINCT, which is what lets everything here be keyed on type.
/// Access is either by visitation - one handler per alternative, exhaustive - or by naming the alternative's
/// type: is<T>, as<T>, try_as<T>, take<T>, try_take<T>.
/// A T that is not an alternative is a compile error rather than a false is<T>().
/// Trivially copyable and trivially destructible whenever every alternative is.
/// Assignment destroys the active alternative and constructs the new one in its place, which is not
/// exception-safe - an alternative whose move constructor can throw is not supported, since we do not
/// model a valueless state to fall back to.
/// Usage:
///   cc::variant<int, cc::string> v = 42;
///   v.emplace<1>("hello");
///   if (auto const* s = v.try_as<cc::string>())
///       use(*s);
///   auto s = v.visit([](int i) { return cc::to_string(i); },
///                    [](cc::string const& s) { return s; });
template <class... Ts>
struct cc::variant
{
    static_assert(sizeof...(Ts) > 0, "variant needs at least one alternative");
    static_assert((std::is_object_v<Ts> && ...),
                  "variant alternatives must be object types (no references, void or functions)");
    static_assert((!std::is_const_v<Ts> && ...),
                  "variant alternatives must not be const - they are constructed and destroyed in place");
    static_assert(((cc::impl::variant_exact_count<Ts, Ts...> == 1) && ...),
                  "variant alternatives must be pairwise distinct - access is keyed on type");

private:
    using storage_t = cc::impl::variant_storage<Ts...>;

    static constexpr std::size_t count = sizeof...(Ts);
    static constexpr bool all_trivially_copyable = (std::is_trivially_copyable_v<Ts> && ...);
    static constexpr bool all_trivially_destructible = (std::is_trivially_destructible_v<Ts> && ...);

    // queries
public:
    /// Number of alternatives.
    static constexpr isize alternative_count = isize(sizeof...(Ts));

    /// The type of the alternative at index I.
    template <std::size_t I>
    using alternative_t = cc::impl::variant_alternative_t<I, Ts...>;

    /// Index of the active alternative, always valid since there is no valueless state.
    [[nodiscard]] constexpr isize index() const { return isize(_index); }

    // visitation
public:
    /// Invokes the active alternative's handler, forwarding this variant's value category.
    /// Several handlers are combined into one overload set, so the common spelling is
    /// v.visit([](int i) { … }, [](cc::string& s) { … }) with one lambda per alternative.
    /// Every alternative must yield the same return type.
    /// Uses deducing this (C++23), so a handler sees T&, T const& or T&& to match.
    template <class... Fs>
    constexpr decltype(auto) visit(this auto&& self, Fs&&... fs)
    {
        static_assert(sizeof...(Fs) > 0, "visit needs at least one handler");

        using self_t = decltype(self);
        auto&& visitor = cc::impl::variant_make_visitor(cc::forward<Fs>(fs)...);

        return cc::impl::variant_dispatch<count>( //
            std::size_t(self._index),
            [&](auto ic) -> decltype(auto)
            {
                return cc::invoke(
                    visitor, cc::impl::variant_alternative<decltype(ic)::value>(static_cast<self_t&&>(self)._storage));
            });
    }

    // type-based access
public:
    /// True when T is the active alternative.
    /// T must be one of the alternatives - asking for an unrelated type does not compile.
    template <class T>
        requires(cc::impl::variant_exact_count<T, Ts...> == 1)
    [[nodiscard]] constexpr bool is() const
    {
        return _index == index_t(cc::impl::variant_exact_index<T, Ts...>());
    }

    /// The active alternative, which must be T.
    /// Yields T&, T const& or T&& to match the variant's own value category.
    template <class T>
        requires(cc::impl::variant_exact_count<T, Ts...> == 1)
    [[nodiscard]] constexpr auto&& as(this auto&& self)
    {
        constexpr std::size_t idx = cc::impl::variant_exact_index<T, Ts...>();

        using self_t = decltype(self);
        CC_ASSERT(self._index == index_t(idx), "variant holds a different alternative");
        return cc::impl::variant_alternative<idx>(static_cast<self_t&&>(self)._storage);
    }

    /// A pointer to the active alternative, or null when the variant holds a different one.
    /// A pointer rather than an optional, so nothing is copied on the way out.
    /// There is deliberately no rvalue overload - the pointer would outlive the temporary it points into.
    template <class T>
        requires(cc::impl::variant_exact_count<T, Ts...> == 1)
    [[nodiscard]] constexpr T* try_as() &
    {
        constexpr std::size_t idx = cc::impl::variant_exact_index<T, Ts...>();
        return _index == index_t(idx) ? &cc::impl::variant_alternative<idx>(_storage) : nullptr;
    }

    template <class T>
        requires(cc::impl::variant_exact_count<T, Ts...> == 1)
    [[nodiscard]] constexpr T const* try_as() const&
    {
        constexpr std::size_t idx = cc::impl::variant_exact_index<T, Ts...>();
        return _index == index_t(idx) ? &cc::impl::variant_alternative<idx>(_storage) : nullptr;
    }

    /// Moves the active alternative, which must be T, out by value.
    /// The variant stays valid and keeps its index - it now holds a moved-from T, exactly as a moved-from
    /// variant does.
    /// There is no valueless state to fall into.
    template <class T>
        requires(cc::impl::variant_exact_count<T, Ts...> == 1 && std::is_move_constructible_v<T>)
    [[nodiscard]] constexpr T take()
    {
        constexpr std::size_t idx = cc::impl::variant_exact_index<T, Ts...>();

        CC_ASSERT(_index == index_t(idx), "variant holds a different alternative");
        return T(cc::move(cc::impl::variant_alternative<idx>(_storage)));
    }

    /// take, but nullopt instead of an assert when the variant holds a different alternative.
    /// An optional rather than a pointer here, since the value has to be moved somewhere anyway.
    template <class T>
        requires(cc::impl::variant_exact_count<T, Ts...> == 1 && std::is_move_constructible_v<T>)
    [[nodiscard]] constexpr cc::optional<T> try_take()
    {
        constexpr std::size_t idx = cc::impl::variant_exact_index<T, Ts...>();

        if (_index != index_t(idx))
            return cc::nullopt;
        return cc::optional<T>(cc::move(cc::impl::variant_alternative<idx>(_storage)));
    }

    // construction
public:
    /// Default-constructs alternative 0, since there is no empty state to fall back to.
    constexpr variant()
        requires(std::is_default_constructible_v<alternative_t<0>>)
    {
        new (cc::placement_new, &cc::impl::variant_alternative<0>(_storage)) alternative_t<0>();
    }

    /// Constructs the one alternative whose type is EXACTLY U, after stripping cv and references.
    /// Deliberately exact rather than best-match: variant<bool, cc::string> never silently takes a string
    /// literal as bool, and an ambiguous or merely convertible argument is a compile error instead.
    /// Reach for create_emplaced<I> when a conversion is intended.
    template <class U>
        requires(cc::impl::variant_exact_count<std::remove_cvref_t<U>, Ts...> == 1)
    constexpr variant(U&& value) : _index(index_t(cc::impl::variant_exact_index<std::remove_cvref_t<U>, Ts...>()))
    {
        constexpr std::size_t idx = cc::impl::variant_exact_index<std::remove_cvref_t<U>, Ts...>();
        new (cc::placement_new, &cc::impl::variant_alternative<idx>(_storage))
            std::remove_cvref_t<U>(cc::forward<U>(value));
    }

    /// Creates a variant with alternative I constructed in place from args.
    /// Returns a prvalue, so this works for immovable alternatives too.
    template <std::size_t I, class... Args>
    [[nodiscard]] static constexpr variant create_emplaced(Args&&... args)
    {
        static_assert(I < count, "variant alternative index out of range");
        static_assert(std::is_constructible_v<alternative_t<I>, Args...>, "alternative I must be constructible from "
                                                                          "Args...");

        return variant(cc::impl::variant_index_tag<I>{}, cc::forward<Args>(args)...);
    }

    // mutation
public:
    /// Destroys the active alternative and constructs alternative I in its place.
    /// args must NOT alias the currently active alternative, which is destroyed first.
    template <std::size_t I, class... Args>
    constexpr alternative_t<I>& emplace(Args&&... args)
    {
        static_assert(I < count, "variant alternative index out of range");
        static_assert(std::is_constructible_v<alternative_t<I>, Args...>, "alternative I must be constructible from "
                                                                          "Args...");

        impl_destroy_active();
        new (cc::placement_new, &cc::impl::variant_alternative<I>(_storage)) alternative_t<I>(cc::forward<Args>(args)...);
        _index = index_t(I);
        return cc::impl::variant_alternative<I>(_storage);
    }

    // trivial copy/move/destroy - defaulted when every alternative allows bitwise operations
public:
    variant(variant&&)
        requires(all_trivially_copyable)
    = default;
    variant(variant const&)
        requires(all_trivially_copyable)
    = default;
    variant& operator=(variant&&)
        requires(all_trivially_copyable)
    = default;
    variant& operator=(variant const&)
        requires(all_trivially_copyable)
    = default;
    ~variant()
        requires(all_trivially_destructible)
    = default;

    // non-trivial copy/move/destroy
public:
    constexpr variant(variant&& rhs) noexcept
        requires(!all_trivially_copyable && (std::is_move_constructible_v<Ts> && ...))
      : _index(rhs._index)
    {
        impl_construct_active_from(cc::move(rhs._storage));
    }

    constexpr variant(variant const& rhs)
        requires(!all_trivially_copyable && (std::is_copy_constructible_v<Ts> && ...))
      : _index(rhs._index)
    {
        impl_construct_active_from(rhs._storage);
    }

    /// Destroys the active alternative, then constructs rhs's in its place - it does not assign through,
    /// not even when both sides hold the same alternative.
    constexpr variant& operator=(variant&& rhs) noexcept
        requires(!all_trivially_copyable && (std::is_move_constructible_v<Ts> && ...))
    {
        if (this != &rhs)
        {
            impl_destroy_active();
            _index = rhs._index;
            impl_construct_active_from(cc::move(rhs._storage));
        }
        return *this;
    }

    constexpr variant& operator=(variant const& rhs)
        requires(!all_trivially_copyable && (std::is_copy_constructible_v<Ts> && ...))
    {
        if (this != &rhs)
        {
            impl_destroy_active();
            _index = rhs._index;
            impl_construct_active_from(rhs._storage);
        }
        return *this;
    }

    constexpr ~variant()
        requires(!all_trivially_destructible)
    {
        impl_destroy_active();
    }

    // comparison
public:
    /// Equal when the indices match and the active alternatives compare equal.
    [[nodiscard]] friend constexpr bool operator==(variant const& lhs, variant const& rhs)
        requires((requires(Ts const& v) { bool(v == v); }) && ...)
    {
        if (lhs._index != rhs._index)
            return false;

        return cc::impl::variant_dispatch<count>( //
            std::size_t(lhs._index),
            [&](auto ic)
            {
                constexpr std::size_t I = decltype(ic)::value;
                return bool(cc::impl::variant_alternative<I>(lhs._storage)
                            == cc::impl::variant_alternative<I>(rhs._storage));
            });
    }

    // hashing
public:
    /// Structural hash combining the index with the active alternative.
    [[nodiscard]] friend constexpr u64 hash(variant const& v)
    {
        return cc::impl::variant_dispatch<count>( //
            std::size_t(v._index), [&](auto ic)
            { return cc::make_hash(v.index(), cc::impl::variant_alternative<decltype(ic)::value>(v._storage)); });
    }

    // implementation
private:
    template <std::size_t I, class... Args>
    constexpr explicit variant(cc::impl::variant_index_tag<I>, Args&&... args) : _index(index_t(I))
    {
        new (cc::placement_new, &cc::impl::variant_alternative<I>(_storage)) alternative_t<I>(cc::forward<Args>(args)...);
    }

    /// Constructs the alternative _index names from the matching alternative of another storage.
    /// _index must already be set, and this variant's storage must hold nothing.
    template <class StorageT>
    constexpr void impl_construct_active_from(StorageT&& rhs_storage)
    {
        cc::impl::variant_dispatch<count>( //
            std::size_t(_index),
            [&](auto ic)
            {
                constexpr std::size_t I = decltype(ic)::value;
                new (cc::placement_new, &cc::impl::variant_alternative<I>(_storage))
                    alternative_t<I>(cc::impl::variant_alternative<I>(static_cast<StorageT&&>(rhs_storage)));
            });
    }

    constexpr void impl_destroy_active()
    {
        if constexpr (!all_trivially_destructible)
            cc::impl::variant_dispatch<count>( //
                std::size_t(_index),
                [&](auto ic)
                {
                    using alt_t = alternative_t<decltype(ic)::value>;
                    if constexpr (!std::is_trivially_destructible_v<alt_t>)
                        cc::impl::variant_alternative<decltype(ic)::value>(_storage).~alt_t();
                });
    }

    // members
private:
    /// Smallest unsigned type that indexes the alternatives and still fits the storage's tail padding.
    using index_t = cc::impl::small_size_t<sizeof...(Ts) - 1, alignof(storage_t)>;

    /// Storage first, so the alternatives sit at offset 0 and the tag lands in tail padding.
    storage_t _storage;
    index_t _index = 0;
};
