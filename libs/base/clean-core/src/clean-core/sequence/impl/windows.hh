#pragma once

#include <clean-core/sequence/impl/call.hh>
#include <clean-core/sequence/source.hh>

#include <utility>

// The adaptor sources that group neighbouring elements: windows (pairwise), chunks and step_by.
// Each runs as an index loop over a source that knows its positions, and buffers what it needs otherwise.

namespace cc::impl
{
template <class T, std::size_t>
using seq_repeat_type = T;

/// The pack of N items a window or chunk yields: each item of the input, by reference where the input has references.
/// A prvalue input is buffered, so its window holds const references into that buffer, valid until the next step.
template <class ItemT, class Is>
struct seq_group_pack;
template <class ItemT, std::size_t... Is>
struct seq_group_pack<ItemT, std::index_sequence<Is...>>
{
    using ref_t = std::conditional_t<std::is_reference_v<ItemT>, ItemT, ItemT const&>;
    using type = seq_pack<seq_repeat_type<ref_t, Is>...>;
};

/// Every N consecutive elements, overlapping: windows<2> is pairwise.
template <class S, std::size_t N>
struct window_source
{
    static_assert(N > 0, "a window has at least one element");

    using inner_item_t = typename S::item_t;
    using item_t = typename seq_group_pack<inner_item_t, std::make_index_sequence<N>>::type;
    using ref_t = typename seq_group_pack<inner_item_t, std::make_index_sequence<N>>::ref_t;
    static constexpr bool is_indexed = seq_has_at<S>;
    static constexpr bool is_borrowed = is_indexed && seq_is_borrowed<S> && std::is_reference_v<inner_item_t>;
    static constexpr bool is_infinite = seq_is_infinite<S>;

    S _inner;
    isize _front = 0; // windows taken from the front
    isize _back = 0;  // windows taken from the back
    seq_next_t<inner_item_t> _slots[is_indexed ? 1 : N];
    bool _filled = false;

    constexpr explicit window_source(S&& inner) : _inner(cc::move(inner)) {}

    template <std::size_t... Is>
    constexpr item_t _window_at(isize first, std::index_sequence<Is...>)
    {
        return impl::seq_make_pack<seq_repeat_type<ref_t, Is>...>(_inner.at(first + isize(Is))...);
    }
    template <std::size_t... Is>
    constexpr item_t _buffered(std::index_sequence<Is...>)
    {
        return impl::seq_make_pack<seq_repeat_type<ref_t, Is>...>(impl::seq_peek(_slots[Is])...);
    }

    // pulls until N are buffered (the first time) or one more is (after); false when the input ran out
    constexpr bool _advance_buffer()
    {
        if (!_filled)
        {
            for (std::size_t k = 0; k < N; ++k)
            {
                _slots[k] = _inner.next();
                if (!_slots[k])
                    return false;
            }
            _filled = true;
            return true;
        }
        auto r = _inner.next();
        if (!r)
            return false;
        for (std::size_t k = 0; k + 1 < N; ++k)
            _slots[k] = cc::move(_slots[k + 1]);
        _slots[N - 1] = cc::move(r);
        return true;
    }

    [[nodiscard]] constexpr isize size() const
        requires is_indexed
    {
        auto const windows = _inner.size() - isize(N) + 1;
        auto const left = windows - _front - _back;
        return left < 0 ? 0 : left;
    }
    [[nodiscard]] constexpr bool is_empty() const
        requires is_indexed
    {
        return this->size() == 0;
    }
    [[nodiscard]] constexpr item_t at(isize i)
        requires is_indexed
    {
        return this->_window_at(_front + i, std::make_index_sequence<N>{});
    }

    [[nodiscard]] constexpr seq_slot<item_t> next()
    {
        if constexpr (is_indexed)
        {
            if (this->size() == 0)
                return {};
            auto const first = _front++;
            return seq_slot<item_t>(seq_emplace, [&] { return this->_window_at(first, std::make_index_sequence<N>{}); });
        }
        else
        {
            if (!this->_advance_buffer())
                return {};
            return seq_slot<item_t>(seq_emplace, [&] { return this->_buffered(std::make_index_sequence<N>{}); });
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if constexpr (is_indexed)
        {
            auto const n = this->size();
            for (isize i = 0; i < n; ++i)
                if (step(this->_window_at(_front + i, std::make_index_sequence<N>{})))
                    return true;
            return false;
        }
        else
        {
            while (this->_advance_buffer())
                if (step(this->_buffered(std::make_index_sequence<N>{})))
                    return true;
            return false;
        }
    }

    [[nodiscard]] constexpr seq_slot<item_t> next_back()
        requires is_indexed
    {
        if (this->size() == 0)
            return {};
        ++_back;
        auto const first = _front + this->size();
        return seq_slot<item_t>(seq_emplace, [&] { return this->_window_at(first, std::make_index_sequence<N>{}); });
    }
};

/// Consecutive groups of N, not overlapping; a trailing group of fewer than N is dropped.
template <class S, std::size_t N>
struct chunk_source
{
    static_assert(N > 0, "a chunk has at least one element");

    using inner_item_t = typename S::item_t;
    using item_t = typename seq_group_pack<inner_item_t, std::make_index_sequence<N>>::type;
    using ref_t = typename seq_group_pack<inner_item_t, std::make_index_sequence<N>>::ref_t;
    static constexpr bool is_indexed = seq_has_at<S>;
    static constexpr bool is_borrowed = is_indexed && seq_is_borrowed<S> && std::is_reference_v<inner_item_t>;
    static constexpr bool is_infinite = seq_is_infinite<S>;

    S _inner;
    isize _front = 0;
    isize _back = 0;
    seq_next_t<inner_item_t> _slots[is_indexed ? 1 : N];

    constexpr explicit chunk_source(S&& inner) : _inner(cc::move(inner)) {}

    template <std::size_t... Is>
    constexpr item_t _chunk_at(isize chunk, std::index_sequence<Is...>)
    {
        return impl::seq_make_pack<seq_repeat_type<ref_t, Is>...>(_inner.at(chunk * isize(N) + isize(Is))...);
    }
    template <std::size_t... Is>
    constexpr item_t _buffered(std::index_sequence<Is...>)
    {
        return impl::seq_make_pack<seq_repeat_type<ref_t, Is>...>(impl::seq_peek(_slots[Is])...);
    }
    constexpr bool _fill()
    {
        for (std::size_t k = 0; k < N; ++k)
        {
            _slots[k] = _inner.next();
            if (!_slots[k])
                return false;
        }
        return true;
    }

    [[nodiscard]] constexpr isize size() const
        requires is_indexed
    {
        return _inner.size() / isize(N) - _front - _back;
    }
    [[nodiscard]] constexpr bool is_empty() const
        requires is_indexed
    {
        return this->size() == 0;
    }
    [[nodiscard]] constexpr item_t at(isize i)
        requires is_indexed
    {
        return this->_chunk_at(_front + i, std::make_index_sequence<N>{});
    }

    [[nodiscard]] constexpr seq_slot<item_t> next()
    {
        if constexpr (is_indexed)
        {
            if (this->size() == 0)
                return {};
            auto const chunk = _front++;
            return seq_slot<item_t>(seq_emplace, [&] { return this->_chunk_at(chunk, std::make_index_sequence<N>{}); });
        }
        else
        {
            if (!this->_fill())
                return {};
            return seq_slot<item_t>(seq_emplace, [&] { return this->_buffered(std::make_index_sequence<N>{}); });
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if constexpr (is_indexed)
        {
            auto const n = this->size();
            for (isize i = 0; i < n; ++i)
                if (step(this->_chunk_at(_front + i, std::make_index_sequence<N>{})))
                    return true;
            return false;
        }
        else
        {
            while (this->_fill())
                if (step(this->_buffered(std::make_index_sequence<N>{})))
                    return true;
            return false;
        }
    }

    [[nodiscard]] constexpr seq_slot<item_t> next_back()
        requires is_indexed
    {
        if (this->size() == 0)
            return {};
        ++_back;
        auto const chunk = _front + this->size();
        return seq_slot<item_t>(seq_emplace, [&] { return this->_chunk_at(chunk, std::make_index_sequence<N>{}); });
    }
};

/// Every k-th element, from the first.
template <class S>
struct step_source
{
    using item_t = typename S::item_t;
    static constexpr bool is_indexed = seq_has_at<S>;
    static constexpr bool is_non_empty = seq_is_non_empty<S>;
    static constexpr bool is_borrowed = seq_is_borrowed<S>;
    static constexpr bool is_expiring = seq_is_expiring<S>;
    static constexpr bool is_infinite = seq_is_infinite<S>;

    S _inner;
    isize _k;
    isize _front = 0;
    isize _back = 0;
    bool _started = false;

    constexpr step_source(S&& inner, isize k) : _inner(cc::move(inner)), _k(k)
    {
        CC_ASSERT(k > 0, "step_by(k) needs k >= 1");
    }

    [[nodiscard]] constexpr isize size() const
        requires is_indexed
    {
        return (_inner.size() + _k - 1) / _k - _front - _back;
    }
    [[nodiscard]] constexpr bool is_empty() const
        requires is_indexed
    {
        return this->size() == 0;
    }
    [[nodiscard]] constexpr item_t at(isize i)
        requires is_indexed
    {
        return _inner.at((_front + i) * _k);
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next()
    {
        if constexpr (is_indexed)
        {
            if (this->size() == 0)
                return {};
            auto const i = _front++;
            return impl::seq_make_next<item_t>([&]() -> item_t { return _inner.at(i * _k); });
        }
        else
        {
            if (_started)
                for (isize s = 1; s < _k; ++s)
                    if (!_inner.next())
                        return {};
            _started = true;
            return _inner.next();
        }
    }

    [[nodiscard]] constexpr bool try_fold(auto&& step)
    {
        if constexpr (is_indexed)
        {
            auto const n = this->size();
            for (isize i = 0; i < n; ++i)
                if (step(_inner.at((_front + i) * _k)))
                    return true;
            return false;
        }
        else
        {
            isize skip = _started ? _k - 1 : 0;
            _started = true;
            return impl::seq_try_fold(_inner,
                                      [&]<class X>(X&& x) -> bool
                                      {
                                          if (skip > 0)
                                          {
                                              --skip;
                                              return false;
                                          }
                                          skip = _k - 1;
                                          return step(cc::forward<X>(x));
                                      });
        }
    }

    [[nodiscard]] constexpr seq_next_t<item_t> next_back()
        requires is_indexed
    {
        if (this->size() == 0)
            return {};
        ++_back;
        auto const i = _front + this->size();
        return impl::seq_make_next<item_t>([&]() -> item_t { return _inner.at(i * _k); });
    }
};
} // namespace cc::impl
