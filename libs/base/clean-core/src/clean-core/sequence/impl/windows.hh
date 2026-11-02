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

/// How a window or chunk over S holds its items, and the pack of N it yields.
/// Indexed, it reads them through at() and never steps S: a reference stays one, a prvalue is held in the pack by value.
/// Buffered, it keeps the last next() results, and an item holding references (a reference, or a zip's pack of them)
/// is kept as it is only from a borrowed source: a reference into the source itself may die at the next step,
/// as flatten's owned inner containers do, so it is copied otherwise.
/// A buffered value's pack then holds const references into the buffer, valid until the next step.
template <class S, bool IsIndexed, class Is>
struct seq_group;
template <class S, bool IsIndexed, std::size_t... Is>
struct seq_group<S, IsIndexed, std::index_sequence<Is...>>
{
    using inner_item_t = typename S::item_t;
    static constexpr bool keeps_item = IsIndexed || !seq_holds_references<inner_item_t> || seq_is_borrowed<S>;

    // what the buffer holds per item, and how the pack names it
    using buffered_t = std::conditional_t<keeps_item, inner_item_t, typename seq_owned<inner_item_t>::type>;
    using ref_t = std::conditional_t<std::is_reference_v<buffered_t> || IsIndexed, buffered_t, buffered_t const&>;
    using type = seq_pack<seq_repeat_type<ref_t, Is>...>;

    // one next() result of S as the buffer holds it
    template <class R>
    [[nodiscard]] static constexpr seq_next_t<buffered_t> buffer(R&& r)
    {
        if constexpr (keeps_item)
            return cc::move(r);
        else
        {
            if (!r)
                return {};
            return seq_next_t<buffered_t>(
                seq_emplace,
                [&] { return seq_owned<inner_item_t>::template make<seq_is_expiring<S>>(impl::seq_get(r)); });
        }
    }
};

/// Every N consecutive elements, overlapping: windows<2> is pairwise.
/// Each element is in up to N windows, so it is an index loop only where at() evaluates nothing, and buffers otherwise:
/// a map's function is still called once per element.
template <class S, std::size_t N>
struct window_source
{
    static_assert(N > 0, "a window has at least one element");

    static constexpr bool is_indexed = seq_has_pure_at<S>;
    using group = seq_group<S, is_indexed, std::make_index_sequence<N>>;
    using item_t = typename group::type;
    using ref_t = typename group::ref_t;
    static constexpr bool is_infinite = seq_is_infinite<S>;
    static constexpr bool has_pure_at = is_indexed;

    S _inner;
    isize _front = 0; // windows taken from the front
    isize _back = 0;  // windows taken from the back
    seq_next_t<typename group::buffered_t> _slots[is_indexed ? 1 : N];
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
                _slots[k] = group::buffer(_inner.next());
                if (!_slots[k])
                    return false;
            }
            _filled = true;
            return true;
        }
        auto r = group::buffer(_inner.next());
        if (!r)
            return false;
        for (std::size_t k = 0; k + 1 < N; ++k)
            _slots[k] = cc::move(_slots[k + 1]);
        _slots[N - 1] = cc::move(r);
        return true;
    }

    // buffered, each element after the first N - 1 completes one window
    [[nodiscard]] constexpr isize size() const
        requires seq_has_size<S>
    {
        auto const windows = _filled ? _inner.size() : _inner.size() - isize(N) + 1;
        auto const left = windows - _front - _back;
        return left < 0 ? 0 : left;
    }
    [[nodiscard]] constexpr bool is_empty() const
        requires seq_has_size<S>
    {
        return this->size() == 0;
    }
    [[nodiscard]] constexpr item_t at(isize i)
        requires is_indexed
    {
        return this->_window_at(_front + i, std::make_index_sequence<N>{});
    }
    constexpr void truncate(isize n)
        requires is_indexed
    {
        auto const size = this->size();
        _back += size - impl::seq_min(n, size);
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
/// Each element is in one chunk, so any source with positions is an index loop.
template <class S, std::size_t N>
struct chunk_source
{
    static_assert(N > 0, "a chunk has at least one element");

    static constexpr bool is_indexed = seq_has_at<S>;
    using group = seq_group<S, is_indexed, std::make_index_sequence<N>>;
    using item_t = typename group::type;
    using ref_t = typename group::ref_t;
    static constexpr bool is_infinite = seq_is_infinite<S>;
    static constexpr bool has_pure_at = seq_has_pure_at<S>;

    S _inner;
    isize _front = 0;
    isize _back = 0;
    seq_next_t<typename group::buffered_t> _slots[is_indexed ? 1 : N];

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
            _slots[k] = group::buffer(_inner.next());
            if (!_slots[k])
                return false;
        }
        return true;
    }

    // buffered, the input has only ever been stepped by whole chunks
    [[nodiscard]] constexpr isize size() const
        requires seq_has_size<S>
    {
        auto const left = _inner.size() / isize(N) - _front - _back;
        return left < 0 ? 0 : left;
    }
    [[nodiscard]] constexpr bool is_empty() const
        requires seq_has_size<S>
    {
        return this->size() == 0;
    }
    [[nodiscard]] constexpr item_t at(isize i)
        requires is_indexed
    {
        return this->_chunk_at(_front + i, std::make_index_sequence<N>{});
    }
    constexpr void truncate(isize n)
        requires is_indexed
    {
        auto const size = this->size();
        _back += size - impl::seq_min(n, size);
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
    static constexpr bool has_pure_at = seq_has_pure_at<S>;

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
        auto const n = _inner.size();
        auto const left = (n == 0 ? 0 : (n - 1) / _k + 1) - _front - _back;
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
        return _inner.at((_front + i) * _k);
    }
    constexpr void truncate(isize n)
        requires is_indexed
    {
        auto const size = this->size();
        _back += size - impl::seq_min(n, size);
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
