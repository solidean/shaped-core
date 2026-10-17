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
//   bool contains(auto const& v) const   // optional: membership without a scan
//   begin(), end()                       // optional: the remaining elements as a range, which range-for then uses directly
//   static constexpr bool is_non_empty   // optional, default false: at least one element before the first next()
//   static constexpr bool is_borrowed    // optional, default false: references stay valid after the source dies
//   static constexpr bool is_expiring    // optional, default false: the source owns its elements, so they may be moved out
//
// next() may be called any number of times, then try_fold at most once, after which the source is spent.
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
template <class T>
struct seq_slot
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>, "a prvalue item must be a non-const object type");

public:
    constexpr seq_slot() {}

    template <class F>
    constexpr explicit seq_slot(seq_emplace_t, F&& make) : _value(cc::forward<F>(make)()), _has_value(true)
    {
    }

    [[nodiscard]] constexpr explicit operator bool() const { return _has_value; }
    [[nodiscard]] constexpr T& operator*() & { return _value; }

    constexpr seq_slot(seq_slot&&)
        requires std::is_trivially_copyable_v<T>
    = default;
    constexpr seq_slot& operator=(seq_slot&&)
        requires std::is_trivially_copyable_v<T>
    = default;
    constexpr ~seq_slot()
        requires std::is_trivially_destructible_v<T>
    = default;

    constexpr seq_slot(seq_slot&& rhs) noexcept
        requires(!std::is_trivially_copyable_v<T>)
      : _has_value(rhs._has_value)
    {
        if (_has_value)
            new (cc::placement_new, &_value) T(cc::move(rhs._value));
    }
    constexpr seq_slot& operator=(seq_slot&& rhs) noexcept
        requires(!std::is_trivially_copyable_v<T>)
    {
        if (this != &rhs)
        {
            if (_has_value)
                _value.~T();
            _has_value = rhs._has_value;
            if (_has_value)
                new (cc::placement_new, &_value) T(cc::move(rhs._value));
        }
        return *this;
    }
    constexpr ~seq_slot()
        requires(!std::is_trivially_destructible_v<T>)
    {
        if (_has_value)
            _value.~T();
    }

    seq_slot(seq_slot const&) = delete;
    seq_slot& operator=(seq_slot const&) = delete;

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

struct seq_probe_step
{
    template <class X>
    bool operator()(X&&) const;
};
template <class S>
concept seq_has_try_fold = requires(S& s) { requires std::is_same_v<decltype(s.try_fold(seq_probe_step{})), bool>; };

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

//
// built-in sources
//

/// The source over contiguous storage, which every contiguous container and view hands out.
template <class T>
struct span_source
{
    using item_t = T&;
    static constexpr bool is_borrowed = true;

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
};

/// The half-open integer range [begin, end), an index sequence.
struct iota_source
{
    using item_t = isize;
    static constexpr bool is_borrowed = true;

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
};

/// A source that owns its container, which is what values() on an rvalue container hands out.
/// It addresses by index rather than pointer, so moving the source is safe even for inline storage.
template <class ContainerT>
struct owning_source
{
    using item_t = std::remove_pointer_t<decltype(std::declval<ContainerT&>().data())>&;
    static constexpr bool is_expiring = true;

    ContainerT _container;
    isize _i = 0;

    constexpr explicit owning_source(ContainerT&& c) : _container(cc::move(c)) {}

    [[nodiscard]] constexpr auto* next()
    {
        if (_i == _container.size())
            return decltype(_container.data())(nullptr);
        auto* const p = _container.data() + _i++;
        [[assume(p != nullptr)]];
        return p;
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        auto* const p = _container.data() + _i;
        auto const n = _container.size() - _i;
        for (isize i = 0; i < n; ++i)
            if (step(p[i]))
                return true;
        return false;
    }

    [[nodiscard]] constexpr isize size() const { return _container.size() - _i; }
    [[nodiscard]] constexpr bool is_empty() const { return _i == _container.size(); }

    [[nodiscard]] constexpr auto* begin() { return _container.data() + _i; }
    [[nodiscard]] constexpr auto* end() { return _container.data() + _container.size(); }
};

/// A source known to be non-empty, by construction or by a check already made.
/// It is the source itself with the trait flipped, so it forwards every capability for free.
template <class S>
struct non_empty_source : S
{
    static constexpr bool is_non_empty = true;

    constexpr explicit non_empty_source(S&& s) : S(cc::move(s)) {}
};

/// T, spelled so the name depends on D.
/// A template naming a type that is complete only later in an include cycle (cc::optional, cc::sequence) uses it
/// to defer the lookup to instantiation.
template <class D, class T>
struct seq_dependent
{
    using type = T;
};
} // namespace cc::impl
