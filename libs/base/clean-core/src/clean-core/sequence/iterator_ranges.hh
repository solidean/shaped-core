#pragma once

#include <clean-core/sequence/sequence.hh>

#include <iterator>

// Sequences over begin/end ranges that are not contiguous: std::list, std::map, std::deque, a std::istream_iterator pair.
// cc::values_of, chain and zip take one once this is included; contiguous ranges and C arrays need nothing.
// It is opt-in because <iterator>, which tells a forward iterator from an input one, is too expensive for every container.
// A dereference yielding an rvalue (std::views::as_rvalue) moves each element into its item as it is reached, whatever
// the chain then does with it, so a filter or count over one empties the elements it passes.

namespace cc::impl
{
/// What a dereference that is not an lvalue reference materializes as: the value type where the iterator names one,
/// so a proxy (std::vector<bool>'s) never outlives its range, and the dereference itself decayed otherwise.
template <class It, class DerefT>
struct seq_iterator_value
{
    using type = std::remove_cvref_t<DerefT>;
};
template <class It, class DerefT>
    requires requires { typename std::iter_value_t<It>; }
struct seq_iterator_value<It, DerefT>
{
    using type = std::iter_value_t<It>;
};

/// A begin/end range: whatever *it yields, a reference or a prvalue.
/// An input iterator's reference may point into the iterator, so next() increments only when the following step comes.
template <class It, class End>
struct iterator_source
{
    using deref_t = decltype(*std::declval<It&>());
    // a prvalue or proxy reference (std::vector<bool>) is the iterator's value type, so no proxy outlives its range
    using item_t
        = std::conditional_t<std::is_lvalue_reference_v<deref_t>, deref_t, typename seq_iterator_value<It, deref_t>::type>;
    // an input iterator may hand out a reference into itself (std::istream_iterator), which the next step overwrites;
    // a forward one is taken at its word that it does not, as std::forward_iterator's multipass guarantee requires
    static constexpr bool is_borrowed = std::is_pointer_v<It> || std::forward_iterator<It>;
    static constexpr bool is_random_access = requires(It a, End b, isize i) {
        { b - a };
        a[i];
        a += i;
    };
    static constexpr bool has_pure_at = is_random_access && std::is_lvalue_reference_v<deref_t>;
    static constexpr bool defers_increment = std::is_lvalue_reference_v<item_t> && !is_borrowed;

    mutable It _it;
    End _end;
    mutable bool _pending = false; // the element next() last handed out is still *_it

    constexpr iterator_source(It it, End end) : _it(cc::move(it)), _end(cc::move(end)) {}

    // the increment next() deferred, made now
    constexpr void _settle() const
    {
        if constexpr (defers_increment)
            if (_pending)
            {
                ++_it;
                _pending = false;
            }
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        this->_settle();
        if (_it == _end)
            return {};
        if constexpr (defers_increment)
        {
            _pending = true;
            return &*_it;
        }
        else if constexpr (std::is_lvalue_reference_v<item_t>)
        {
            auto* const p = &*_it;
            ++_it;
            return p;
        }
        else
        {
            auto r = seq_next_t<item_t>(seq_emplace, [&] { return item_t(*_it); });
            ++_it;
            return r;
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        this->_settle();
        for (; _it != _end; ++_it)
            if (step(static_cast<item_t>(*_it)))
                return true;
        return false;
    }

    [[nodiscard]] constexpr auto begin() const
    {
        this->_settle();
        return _it;
    }
    [[nodiscard]] constexpr auto end() const { return _end; }

    [[nodiscard]] constexpr isize size() const
        requires is_random_access
    {
        return isize(_end - _it);
    }
    [[nodiscard]] constexpr bool is_empty() const
    {
        this->_settle();
        return _it == _end;
    }
    [[nodiscard]] constexpr item_t at(isize i) const
        requires is_random_access
    {
        return _it[i];
    }
    constexpr void advance(isize n)
        requires is_random_access
    {
        _it += impl::seq_min(n, this->size());
    }
};

/// A temporary foreign range that is not contiguous, owned through one heap allocation.
/// Its iterators may point into the range object itself (std::list keeps its end sentinel there), so the range must not
/// move once they exist; the allocation is what lets the source move instead.
template <class R>
struct owning_range_source
{
    using it_t = decltype(std::declval<R&>().begin());
    using end_t = decltype(std::declval<R&>().end());
    using inner_t = iterator_source<it_t, end_t>;
    using item_t = typename inner_t::item_t;
    // a temporary range may be a view over elements it does not own (std::views::reverse(v)): only one with an allocator,
    // a std::list or std::map, owns them, so only that moves them out
    static constexpr bool is_expiring = std::is_lvalue_reference_v<item_t> && seq_owns_elements<R>;

    R* _range;
    inner_t _inner;

    constexpr explicit owning_range_source(R&& r) : _range(new R(cc::move(r))), _inner(_range->begin(), _range->end())
    {
    }

    constexpr owning_range_source(owning_range_source&& rhs) noexcept : _range(rhs._range), _inner(cc::move(rhs._inner))
    {
        rhs._range = nullptr;
    }
    owning_range_source(owning_range_source const&) = delete;
    owning_range_source& operator=(owning_range_source const&) = delete;
    owning_range_source& operator=(owning_range_source&&) = delete;
    constexpr ~owning_range_source() { delete _range; }

    [[nodiscard]] constexpr auto next() { return _inner.next(); }
    [[nodiscard]] constexpr bool try_fold(auto&& step) { return _inner.try_fold(step); }
    [[nodiscard]] constexpr bool is_empty() const { return _inner.is_empty(); }
    [[nodiscard]] constexpr isize size() const
        requires inner_t::is_random_access
    {
        return _inner.size();
    }
};

template <class R>
struct seq_iterator_range
{
    template <class X>
    [[nodiscard]] static constexpr auto make(X&& r)
    {
        if constexpr (std::is_lvalue_reference_v<X>)
            return iterator_source<decltype(r.begin()), decltype(r.end())>(r.begin(), r.end());
        else
            return owning_range_source<R>(cc::move(r));
    }
};
} // namespace cc::impl
