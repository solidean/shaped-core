#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>

#include <type_traits>

// The source protocol under cc::sequence, and the sources every container hands out.
// Containers include this before they are defined and cc::sequence only after, so it must not reach optional or span.
//
// A source is what a container or adaptor implements; cc::sequence<Source> is the rich API on top.
//
//   using item_t = ...;                  // what one step yields: T&, T const& or a prvalue T
//   next()                               // required: T* for reference items, impl::seq_slot<T> for prvalues, empty at the end
//   bool try_fold(auto&& step)           // optional: step(item) -> bool, true stops; returns whether it stopped
//   isize size() const                   // optional: the elements left
//   bool is_empty() const                // optional
//   bool contains(auto const& v) const   // optional: membership without a scan, answered for the untouched source
//   begin(), end()                       // optional: the remaining elements as a range, which range-for then uses directly
//   item_t at(isize i)                   // optional, with size(): the i-th remaining element, 0 <= i < size()
//   static constexpr bool has_pure_at    // optional, default false: at(i) evaluates nothing, so asking twice is free
//   void advance(isize n)                // optional, with size(): drop the first min(n, size()) elements unevaluated
//   void truncate(isize n)               // optional, with size(): keep only the first min(n, size()) elements
//   next_back()                          // optional: the last remaining element, as next() returns the first
//   bool try_rfold(auto&& step)          // optional, with next_back(): try_fold from the back
//   static constexpr bool is_non_empty   // optional, default false: at least one element before the first next()
//   static constexpr bool is_borrowed    // optional, default false: references stay valid after the source dies
//   static constexpr bool is_expiring    // optional, default false: the source owns its elements, so they may be moved out
//   static constexpr bool is_infinite    // optional, default false: next() never runs out
//
// next() and next_back() may be called any number of times in any mix, then try_fold or try_rfold at most once,
// after which the source is spent.
// at, advance and truncate address the elements remaining, so they may follow next() and next_back().
// A reference a source hands out may point into the source itself (repeat's value, a temporary fixed_vector it owns):
// it is valid only while that source object lives, so a source is never moved once it has handed one out.
// try_fold must spell its return type `bool`: detection checks the declaration, never the body.
// See libs/base/clean-core/docs/sequence.md.

namespace cc::impl
{
struct seq_emplace_t
{
};
inline constexpr seq_emplace_t seq_emplace = {};

/// The next() result of a source yielding prvalues: empty, or one T.
/// Built from a callable so a prvalue result lands in place without a move.
/// Assignment destroys and reconstructs, so it works for T that cannot be assigned, a source holding a lambda say.
template <class T>
struct seq_slot
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>, "a prvalue item must be a non-const object type");

    static constexpr bool is_trivial = std::is_trivially_copyable_v<T> && std::is_trivially_move_assignable_v<T>;

public:
    constexpr seq_slot() {}

    template <class F>
    constexpr explicit seq_slot(seq_emplace_t, F&& make) : _value(cc::forward<F>(make)()), _has_value(true)
    {
    }

    [[nodiscard]] constexpr explicit operator bool() const { return _has_value; }
    [[nodiscard]] constexpr T& operator*() & { return _value; }

    constexpr seq_slot(seq_slot&&)
        requires is_trivial
    = default;
    constexpr seq_slot& operator=(seq_slot&&)
        requires is_trivial
    = default;
    constexpr ~seq_slot()
        requires std::is_trivially_destructible_v<T>
    = default;

    constexpr seq_slot(seq_slot&& rhs) noexcept
        requires(!is_trivial)
      : _has_value(rhs._has_value)
    {
        if (_has_value)
            new (cc::placement_new, &_value) T(cc::move(rhs._value));
    }
    constexpr seq_slot& operator=(seq_slot&& rhs) noexcept
        requires(!is_trivial)
    {
        if (this != &rhs)
        {
            this->reset();
            _has_value = rhs._has_value;
            if (_has_value)
                new (cc::placement_new, &_value) T(cc::move(rhs._value));
        }
        return *this;
    }
    constexpr ~seq_slot()
        requires(!std::is_trivially_destructible_v<T>)
    {
        this->reset();
    }

    seq_slot(seq_slot const&) = delete;
    seq_slot& operator=(seq_slot const&) = delete;

    constexpr void reset()
    {
        if constexpr (!std::is_trivially_destructible_v<T>)
            if (_has_value)
                _value.~T();
        _has_value = false;
    }

private:
    union
    {
        T _value;
    };
    bool _has_value = false;
};

/// What next() returns for a given item type.
template <class ItemT>
using seq_next_t = std::conditional_t<std::is_reference_v<ItemT>, std::remove_reference_t<ItemT>*, seq_slot<ItemT>>;

/// Hands the item out of a next() result with its value category: the reference itself, or the prvalue as T&&.
template <class T>
[[nodiscard]] constexpr T& seq_get(T* p)
{
    return *p;
}
template <class T>
[[nodiscard]] constexpr T&& seq_get(seq_slot<T>& s)
{
    return cc::move(*s);
}

/// The item in a next() result as an lvalue, for a predicate that must not consume it.
template <class T>
[[nodiscard]] constexpr T& seq_peek(T* p)
{
    return *p;
}
template <class T>
[[nodiscard]] constexpr T& seq_peek(seq_slot<T>& s)
{
    return *s;
}

/// A next() result holding item, built in place.
template <class ItemT, class F>
[[nodiscard]] constexpr seq_next_t<ItemT> seq_make_next(F&& make)
{
    if constexpr (std::is_reference_v<ItemT>)
        return &make();
    else
        return seq_next_t<ItemT>(seq_emplace, cc::forward<F>(make));
}

/// T, spelled so the name depends on D.
/// A template naming a type that is complete only later in an include cycle (cc::optional, cc::sequence) uses it
/// to defer the lookup to instantiation.
template <class D, class T>
struct seq_dependent
{
    using type = T;
};

/// The one door from a sequence to its source, for the adaptors that consume a sequence (chain, zip, flatten).
struct seq_access;

//
// source traits
//

template <class S>
inline constexpr bool seq_is_non_empty = requires { requires S::is_non_empty; };
template <class S>
inline constexpr bool seq_is_borrowed = requires { requires S::is_borrowed; };
template <class S>
inline constexpr bool seq_is_expiring = requires { requires S::is_expiring; };
template <class S>
inline constexpr bool seq_is_infinite = requires { requires S::is_infinite; };

template <class S>
concept seq_has_size = requires(S const& s) { requires std::is_convertible_v<decltype(s.size()), isize>; };
template <class S>
concept seq_has_is_empty = requires(S const& s) { requires std::is_convertible_v<decltype(s.is_empty()), bool>; };
template <class S, class V>
concept seq_has_contains
    = requires(S const& s, V const& v) { requires std::is_convertible_v<decltype(s.contains(v)), bool>; };
template <class S>
concept seq_has_range = requires(S& s) {
    s.begin();
    s.end();
};
template <class S>
concept seq_has_at = seq_has_size<S> && requires(S& s) { s.at(isize(0)); };
template <class S>
concept seq_has_pure_at = seq_has_at<S> && requires { requires S::has_pure_at; };
template <class S>
concept seq_has_advance = seq_has_size<S> && requires(S& s) { s.advance(isize(0)); };
template <class S>
concept seq_has_truncate = seq_has_size<S> && requires(S& s) { s.truncate(isize(0)); };

struct seq_probe_step
{
    template <class X>
    bool operator()(X&&) const;
};
template <class S>
concept seq_has_try_fold = requires(S& s) { requires std::is_same_v<decltype(s.try_fold(seq_probe_step{})), bool>; };
template <class S>
concept seq_has_next_back = requires(S& s) { s.next_back(); };
template <class S>
concept seq_has_try_rfold = requires(S& s) { requires std::is_same_v<decltype(s.try_rfold(seq_probe_step{})), bool>; };

/// The internal fold where the source has one, the next() loop where it does not.
/// step(item) -> bool, true stops; returns whether it stopped, and leaves the source spent.
template <class S, class StepF>
[[nodiscard]] constexpr bool seq_try_fold(S& source, StepF&& step)
{
    if constexpr (seq_has_try_fold<S>)
        return source.try_fold(step);
    else
    {
        while (true)
        {
            auto r = source.next();
            if (!r)
                return false;
            if (step(impl::seq_get(r)))
                return true;
        }
    }
}

/// try_fold from the back: the internal rfold where the source has one, the next_back() loop where it does not.
template <class S, class StepF>
[[nodiscard]] constexpr bool seq_try_rfold(S& source, StepF&& step)
{
    if constexpr (seq_has_try_rfold<S>)
        return source.try_rfold(step);
    else
    {
        while (true)
        {
            auto r = source.next_back();
            if (!r)
                return false;
            if (step(impl::seq_get(r)))
                return true;
        }
    }
}

[[nodiscard]] constexpr isize seq_min(isize a, isize b)
{
    return a < b ? a : b;
}

/// At least one element at compile time: non-empty by trait, or infinite.
template <class S>
inline constexpr bool seq_never_empty = seq_is_non_empty<S> || seq_is_infinite<S>;

/// p, when it points into the bytes of from, as the same offset into to; anything else unchanged.
/// It carries a reference a source handed out into its own storage across a move of that source,
/// which is sound because a source's move keeps each member at its offset.
template <class T, class S>
[[nodiscard]] constexpr T* seq_rebase(T* p, S const& from, S& to)
{
    if consteval
    {
        return p;
    }
    else
    {
        auto const q = reinterpret_cast<u64>(p);     // NOLINT
        auto const b = reinterpret_cast<u64>(&from); // NOLINT
        if (q < b || q >= b + sizeof(S))
            return p;
        return reinterpret_cast<T*>(reinterpret_cast<unsigned char*>(&to) + (q - b)); // NOLINT
    }
}

//
// built-in sources
//

/// The source over contiguous storage, which every contiguous container and view hands out.
template <class T>
struct span_source
{
    using item_t = T&;
    static constexpr bool is_borrowed = true;
    static constexpr bool has_pure_at = true;

    T* _p;
    T* _end;

    constexpr span_source(T* begin, T* end) : _p(begin), _end(end) {}

    [[nodiscard]] constexpr T* next()
    {
        if (_p == _end)
            return nullptr;
        T* const p = _p++;
        [[assume(p != nullptr)]]; // so an external loop tests only the end, never the pointer
        return p;
    }

    // indexed rather than pointer-stepping, so a caller's own index counter fuses with the loop's
    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        auto const p = _p;
        auto const n = _end - _p;
        for (isize i = 0; i < n; ++i)
            if (step(p[i]))
                return true;
        return false;
    }

    [[nodiscard]] constexpr isize size() const { return _end - _p; }
    [[nodiscard]] constexpr bool is_empty() const { return _p == _end; }

    [[nodiscard]] constexpr T* begin() const { return _p; }
    [[nodiscard]] constexpr T* end() const { return _end; }

    [[nodiscard]] constexpr T& at(isize i) const { return _p[i]; }
    constexpr void advance(isize n) { _p += impl::seq_min(n, size()); }
    constexpr void truncate(isize n) { _end = _p + impl::seq_min(n, size()); }

    [[nodiscard]] constexpr T* next_back()
    {
        if (_p == _end)
            return nullptr;
        T* const p = --_end;
        [[assume(p != nullptr)]];
        return p;
    }
    [[nodiscard]] constexpr bool try_rfold(auto&& step)
    {
        auto const p = _p;
        for (isize i = _end - _p - 1; i >= 0; --i)
            if (step(p[i]))
                return true;
        return false;
    }
};

/// The half-open integer range [begin, end), an index sequence.
struct iota_source
{
    using item_t = isize;
    static constexpr bool is_borrowed = true;
    static constexpr bool has_pure_at = true;

    isize _i;
    isize _end;

    constexpr iota_source(isize begin, isize end) : _i(begin), _end(end)
    {
        CC_ASSERT(begin <= end, "an index range must not end before it begins");
    }

    [[nodiscard]] constexpr seq_slot<isize> next()
    {
        if (_i == _end)
            return {};
        return seq_slot<isize>(seq_emplace, [&] { return _i++; });
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        for (auto i = _i, e = _end; i != e; ++i)
            if (step(isize(i)))
                return true;
        return false;
    }

    [[nodiscard]] constexpr isize size() const { return _end - _i; }
    [[nodiscard]] constexpr bool is_empty() const { return _i == _end; }
    [[nodiscard]] constexpr bool contains(isize i) const { return _i <= i && i < _end; }

    [[nodiscard]] constexpr isize at(isize i) const { return _i + i; }
    constexpr void advance(isize n) { _i += impl::seq_min(n, size()); }
    constexpr void truncate(isize n) { _end = _i + impl::seq_min(n, size()); }

    [[nodiscard]] constexpr seq_slot<isize> next_back()
    {
        if (_i == _end)
            return {};
        return seq_slot<isize>(seq_emplace, [&] { return --_end; });
    }
    [[nodiscard]] constexpr bool try_rfold(auto&& step)
    {
        for (auto i = _end, b = _i; i != b; --i)
            if (step(isize(i - 1)))
                return true;
        return false;
    }
};

/// A source that owns its container, which is what values() on an rvalue container hands out.
/// It addresses by index rather than pointer, so moving the source is safe even for inline storage.
template <class ContainerT>
struct owning_source
{
    using item_t = std::remove_pointer_t<decltype(std::declval<ContainerT&>().data())>&;
    static constexpr bool is_expiring = true;
    static constexpr bool has_pure_at = true;

    ContainerT _container;
    isize _i = 0;
    isize _end;

    constexpr explicit owning_source(ContainerT&& c) : _container(cc::move(c)), _end(_container.size()) {}

    [[nodiscard]] constexpr auto* next()
    {
        if (_i == _end)
            return decltype(_container.data())(nullptr);
        auto* const p = _container.data() + _i++;
        [[assume(p != nullptr)]];
        return p;
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        auto* const p = _container.data() + _i;
        auto const n = _end - _i;
        for (isize i = 0; i < n; ++i)
            if (step(p[i]))
                return true;
        return false;
    }

    [[nodiscard]] constexpr isize size() const { return _end - _i; }
    [[nodiscard]] constexpr bool is_empty() const { return _i == _end; }

    [[nodiscard]] constexpr auto* begin() { return _container.data() + _i; }
    [[nodiscard]] constexpr auto* end() { return _container.data() + _end; }

    [[nodiscard]] constexpr item_t at(isize i) { return _container.data()[_i + i]; }
    constexpr void advance(isize n) { _i += impl::seq_min(n, size()); }
    constexpr void truncate(isize n) { _end = _i + impl::seq_min(n, size()); }

    [[nodiscard]] constexpr auto* next_back()
    {
        if (_i == _end)
            return decltype(_container.data())(nullptr);
        auto* const p = _container.data() + --_end;
        [[assume(p != nullptr)]];
        return p;
    }
    [[nodiscard]] constexpr bool try_rfold(auto&& step)
    {
        auto* const p = _container.data();
        for (isize i = _end - 1; i >= _i; --i)
            if (step(p[i]))
                return true;
        return false;
    }
};

/// The source over elements a fixed byte stride apart, which strided_span hands out.
template <class T>
struct strided_source
{
    using item_t = T&;
    using byte_ptr = std::conditional_t<std::is_const_v<T>, unsigned char const*, unsigned char*>;
    static constexpr bool is_borrowed = true;
    static constexpr bool has_pure_at = true;

    byte_ptr _start;
    isize _stride;
    isize _i = 0;
    isize _end;

    constexpr strided_source(T* start, isize stride_bytes, isize size)
      : _start(reinterpret_cast<byte_ptr>(start)), _stride(stride_bytes), _end(size) // NOLINT
    {
    }

    [[nodiscard]] constexpr T& at(isize i) const
    {
        return *reinterpret_cast<T*>(_start + (_i + i) * _stride);
    } // NOLINT

    [[nodiscard]] constexpr T* next()
    {
        if (_i == _end)
            return nullptr;
        T* const p = &this->at(0);
        ++_i;
        [[assume(p != nullptr)]];
        return p;
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        auto const n = _end - _i;
        for (isize i = 0; i < n; ++i)
            if (step(this->at(i)))
                return true;
        return false;
    }

    [[nodiscard]] constexpr isize size() const { return _end - _i; }
    [[nodiscard]] constexpr bool is_empty() const { return _i == _end; }
    constexpr void advance(isize n) { _i += impl::seq_min(n, size()); }
    constexpr void truncate(isize n) { _end = _i + impl::seq_min(n, size()); }

    [[nodiscard]] constexpr T* next_back()
    {
        if (_i == _end)
            return nullptr;
        --_end;
        T* const p = reinterpret_cast<T*>(_start + _end * _stride); // NOLINT
        [[assume(p != nullptr)]];
        return p;
    }
};

/// A source known to be non-empty, by construction or by a check already made.
/// It is the source itself with the trait flipped, so it forwards every capability for free.
/// advance and truncate stay, so every adaptor over it keeps them: take and skip, which use them, drop the trait.
template <class S>
struct non_empty_source : S
{
    static constexpr bool is_non_empty = true;

    constexpr explicit non_empty_source(S&& s) : S(cc::move(s)) {}
};

/// A source that may be empty although S, under it, was typed non-empty: what take and skip of a non-empty one return.
/// Like non_empty_source, it is S itself with the trait flipped.
template <class S>
struct maybe_empty_source : S
{
    static constexpr bool is_non_empty = false;

    constexpr explicit maybe_empty_source(S&& s) : S(cc::move(s)) {}
};

/// S without its non-emptiness: S itself, the source under a non_empty_source, or a maybe_empty_source over it.
template <class S>
struct seq_maybe_empty
{
    using type = std::conditional_t<seq_is_non_empty<S>, maybe_empty_source<S>, S>;
};
template <class S>
struct seq_maybe_empty<non_empty_source<S>>
{
    using type = typename seq_maybe_empty<S>::type;
};

/// A sequence over source, non-empty at compile time when IsNonEmpty.
template <bool IsNonEmpty, class S>
[[nodiscard]] constexpr auto seq_make(S&& source)
{
    using source_t = std::remove_cvref_t<S>;
    if constexpr (IsNonEmpty)
        return cc::sequence<non_empty_source<source_t>>(non_empty_source<source_t>(cc::move(source)));
    else
        return cc::sequence<source_t>(cc::move(source));
}

/// values() of a contiguous container: borrowing over an lvalue, owning over a temporary.
/// A const temporary can be neither borrowed, since it dies, nor moved from, so it is refused.
template <bool IsNonEmpty, class C>
[[nodiscard]] constexpr auto seq_contiguous_values(C&& c)
{
    static_assert(std::is_lvalue_reference_v<C> || !std::is_const_v<std::remove_reference_t<C>>,
                  "values() of a const temporary: it dies with the expression and cannot be moved from, "
                  "so drop the const or bind it to a variable first");
    if constexpr (std::is_lvalue_reference_v<C>)
    {
        using element_t = std::remove_pointer_t<decltype(c.data())>;
        return impl::seq_make<IsNonEmpty>(span_source<element_t>(c.data(), c.data() + c.size()));
    }
    else
        return impl::seq_make<IsNonEmpty>(owning_source<std::remove_cvref_t<C>>(cc::move(c)));
}

/// indices() of an indexed container.
template <bool IsNonEmpty, class C>
[[nodiscard]] constexpr auto seq_index_values(C const& c)
{
    using source_t = typename seq_dependent<C, iota_source>::type;
    return impl::seq_make<IsNonEmpty>(source_t(0, isize(c.size())));
}
} // namespace cc::impl
