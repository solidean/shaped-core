#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh> // cc::sentinel
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/math/bit.hh>

// The shared surface of cc::bitset and cc::fixed_bitset, plus the bit proxy and the index ranges both hand out.
//
// Everything works one WORD at a time, which is the whole point of the type: a query over 4096 bits touches 64 words.
// The word type is a template parameter rather than a fixed u64, because that is what lets fixed_bitset<8> be one byte.
//
// The derived type owns the storage and supplies, publicly or as a friend:
//   WordT* _words_data() / WordT const* _words_data() const     base of the word array
//   isize size() const                                          number of BITS (may be static on a fixed bitset)
//   isize word_count() const                                    words covering those bits, i.e. ceil(size / bits_per_word)
//
// TAIL INVARIANT, relied on by every query here: bits at index >= size() are zero.
// That is what makes any_set() a word compare, set_bit_count() a plain popcount sum, and the set operations
// tail-preserving for free — only the members that write whole words (set_all, toggle_all, and the derived
// type's own resizing) have to restore it, via _mask_tail().

namespace cc::impl
{
/// Byte width of the word backing a bitset of `bit_count` bits: the smallest of 1/2/4/8 that covers it, and 8 beyond 64 bits.
[[nodiscard]] consteval int bitset_word_bytes(u64 bit_count)
{
    int bytes = 1;
    while (bytes < 8 && bit_count > 8 * u64(bytes))
        bytes *= 2;
    return bytes;
}

template <int Bytes>
struct bitset_word_type_of;
template <>
struct bitset_word_type_of<1>
{
    using type = u8;
};
template <>
struct bitset_word_type_of<2>
{
    using type = u16;
};
template <>
struct bitset_word_type_of<4>
{
    using type = u32;
};
template <>
struct bitset_word_type_of<8>
{
    using type = u64;
};

/// Smallest unsigned word type that holds `BitCount` bits, capped at u64.
/// fixed_bitset<8> is therefore one byte and fixed_bitset<100> a pair of u64s.
template <u64 BitCount>
using bitset_word_t = typename bitset_word_type_of<bitset_word_bytes(BitCount)>::type;
} // namespace cc::impl

/// A single bit, handed out by the non-const bitset operator[].
///
/// A bitset does not store addressable bools, so this proxy is what indexing yields — uniformly, for every bitset and
/// every element, which is precisely what std::vector<bool> gets wrong by being the odd one out among vectors.
///
/// `auto b = bs[i]` therefore captures the proxy and keeps tracking the bitset; write `bool b = bs[i]` for a snapshot.
template <class WordT>
struct cc::bit_ref
{
    constexpr bit_ref(WordT* word, WordT mask) : _word(word), _mask(mask) {}

    [[nodiscard]] constexpr operator bool() const { return (*_word & _mask) != 0; }

    constexpr bit_ref& operator=(bool on)
    {
        *_word = on ? WordT(*_word | _mask) : WordT(*_word & WordT(~_mask));
        return *this;
    }

    /// Assigns the VALUE of rhs, not the reference — the proxy convention, and why this hides the implicit copy assignment.
    constexpr bit_ref& operator=(bit_ref const& rhs) { return *this = bool(rhs); }

    constexpr void toggle() { *_word = WordT(*_word ^ _mask); }

private:
    WordT* _word;
    WordT _mask;
};

/// Forward iterator over the indices of the set (`Set == true`) or unset bits of a word array.
///
/// One word is held in `_current` and consumed bit by bit, lowest first, so a sparse bitset costs one step per SET bit
/// rather than one per bit — the reason to iterate a bitset this way at all.
template <class WordT, bool Set>
struct cc::bit_index_iterator
{
    static constexpr isize bits_per_word = isize(8 * sizeof(WordT));

    constexpr bit_index_iterator() = default;
    constexpr bit_index_iterator(WordT const* words, isize bit_count)
      : _words(words), _word_count((bit_count + bits_per_word - 1) / bits_per_word), _bit_count(bit_count)
    {
        if (_word_count > 0)
        {
            _current = _load(0);
            _skip_empty_words();
        }
    }

    [[nodiscard]] constexpr isize operator*() const
    {
        return _word_idx * bits_per_word + isize(cc::count_trailing_zeroes(_current));
    }

    constexpr bit_index_iterator& operator++()
    {
        _current = WordT(_current & WordT(_current - WordT(1))); // clear the lowest set bit
        _skip_empty_words();
        return *this;
    }
    constexpr bit_index_iterator operator++(int)
    {
        auto const copy = *this;
        ++*this;
        return copy;
    }

    /// Exhausted iff no bit is left in the cursor word, which _skip_empty_words guarantees only happens past the end.
    [[nodiscard]] friend constexpr bool operator==(bit_index_iterator const& it, cc::sentinel)
    {
        return it._current == 0;
    }

private:
    /// The bits of word `i` this iteration cares about.
    /// The unset case must mask the tail explicitly: those bits read as zero and would otherwise all be reported.
    [[nodiscard]] constexpr WordT _load(isize i) const
    {
        if constexpr (Set)
            return _words[i]; // the tail invariant already keeps the bits above _bit_count out
        else
        {
            auto const rest = _bit_count - i * bits_per_word;
            auto const valid = rest >= bits_per_word ? WordT(~WordT(0)) : WordT((u64(1) << rest) - 1);
            return WordT(WordT(~_words[i]) & valid);
        }
    }

    constexpr void _skip_empty_words()
    {
        while (_current == 0 && _word_idx + 1 < _word_count)
            _current = _load(++_word_idx);
    }

    WordT const* _words = nullptr;
    isize _word_count = 0;
    isize _bit_count = 0;
    isize _word_idx = 0;
    WordT _current = 0;
};

/// The range returned by bitset::set_indices() / unset_indices(), valid as long as the bitset's storage is.
template <class WordT, bool Set>
struct cc::bit_index_range
{
    constexpr bit_index_range(WordT const* words, isize bit_count) : _words(words), _bit_count(bit_count) {}

    [[nodiscard]] constexpr cc::bit_index_iterator<WordT, Set> begin() const { return {_words, _bit_count}; }
    [[nodiscard]] constexpr cc::sentinel end() const { return {}; }

private:
    WordT const* _words;
    isize _bit_count;
};

/// Mixin implementing the common "packed array of bits" surface area.
///
/// Every runtime bit index is CC_ASSERT-checked, and every operation taking a second bitset requires an equal size():
/// treating a shorter operand as zero-extended would truncate under retain_all_of while growing nothing under
/// set_all_of, and that asymmetry produces a plausible wrong answer where an assert produces a bug report.
template <class ContainerT, class WordT>
struct cc::bitset_container
{
    using word_t = WordT;

    static constexpr isize bits_per_word = isize(8 * sizeof(WordT));
    static constexpr WordT all_ones = WordT(~WordT(0));

    // element access
public:
    /// Returns whether bit i is set.
    /// Precondition: 0 <= i < size().
    [[nodiscard]] constexpr bool operator[](isize i) const { return is_set(i); }

    /// Returns a proxy for bit i; see cc::bit_ref, and note that `auto` captures the proxy rather than a bool.
    /// Precondition: 0 <= i < size().
    [[nodiscard]] constexpr cc::bit_ref<WordT> operator[](isize i)
    {
        CC_ASSERT(0 <= i && i < _size(), "bit index out of bounds");
        return {_words() + _word_of(i), _bit_of(i)};
    }

    [[nodiscard]] constexpr bool is_set(isize i) const
    {
        CC_ASSERT(0 <= i && i < _size(), "bit index out of bounds");
        return (_words()[_word_of(i)] & _bit_of(i)) != 0;
    }

    /// The raw words, lowest index in the least significant bit of word 0.
    [[nodiscard]] constexpr cc::span<WordT const> words_span() const
    {
        return cc::span<WordT const>(_words(), _self().word_count());
    }

    // queries
public:
    [[nodiscard]] constexpr bool any_set() const
    {
        auto const* const w = _words();
        for (isize i = 0; i < _self().word_count(); ++i)
            if (w[i] != 0)
                return true;
        return false;
    }

    [[nodiscard]] constexpr bool none_set() const { return !any_set(); }

    [[nodiscard]] constexpr bool all_set() const
    {
        auto const* const w = _words();
        for (isize i = 0; i < _self().word_count(); ++i)
            if (w[i] != _valid_mask(i))
                return false;
        return true;
    }

    [[nodiscard]] constexpr isize set_bit_count() const
    {
        isize count = 0;
        auto const* const w = _words();
        for (isize i = 0; i < _self().word_count(); ++i)
            count += isize(cc::popcount(w[i]));
        return count;
    }

    [[nodiscard]] constexpr isize unset_bit_count() const { return _size() - set_bit_count(); }

    // single-bit mutation
public:
    constexpr void set(isize i)
    {
        CC_ASSERT(0 <= i && i < _size(), "bit index out of bounds");
        auto& w = _words()[_word_of(i)];
        w = WordT(w | _bit_of(i));
    }

    constexpr void set(isize i, bool on) { on ? set(i) : unset(i); }

    constexpr void unset(isize i)
    {
        CC_ASSERT(0 <= i && i < _size(), "bit index out of bounds");
        auto& w = _words()[_word_of(i)];
        w = WordT(w & WordT(~_bit_of(i)));
    }

    constexpr void toggle(isize i)
    {
        CC_ASSERT(0 <= i && i < _size(), "bit index out of bounds");
        auto& w = _words()[_word_of(i)];
        w = WordT(w ^ _bit_of(i));
    }

    // bulk mutation
public:
    constexpr void set_all()
    {
        auto* const w = _words();
        for (isize i = 0; i < _self().word_count(); ++i)
            w[i] = all_ones;
        _mask_tail();
    }

    constexpr void set_all(bool on) { on ? set_all() : unset_all(); }

    constexpr void unset_all()
    {
        auto* const w = _words();
        for (isize i = 0; i < _self().word_count(); ++i)
            w[i] = 0;
    }

    constexpr void toggle_all()
    {
        auto* const w = _words();
        for (isize i = 0; i < _self().word_count(); ++i)
            w[i] = WordT(w[i] ^ all_ones);
        _mask_tail();
    }

    /// Sets the bits in [start, start + count).
    /// Precondition: 0 <= start, 0 <= count, start + count <= size().
    constexpr void set_range(isize start, isize count)
    {
        _for_each_range_word(start, count, [](WordT& w, WordT mask) { w = WordT(w | mask); });
    }

    constexpr void set_range(isize start, isize count, bool on)
    {
        on ? set_range(start, count) : unset_range(start, count);
    }

    constexpr void unset_range(isize start, isize count)
    {
        _for_each_range_word(start, count, [](WordT& w, WordT mask) { w = WordT(w & WordT(~mask)); });
    }

    constexpr void toggle_range(isize start, isize count)
    {
        _for_each_range_word(start, count, [](WordT& w, WordT mask) { w = WordT(w ^ mask); });
    }

    // search
public:
    /// Index of the lowest set bit at or above `from`, or -1 if there is none.
    /// Precondition: 0 <= from <= size().
    [[nodiscard]] constexpr isize find_first_set(isize from = 0) const { return _find_first<true>(from); }
    [[nodiscard]] constexpr isize find_first_unset(isize from = 0) const { return _find_first<false>(from); }

    /// Index of the highest set bit below `upto`, or -1 if there is none.
    /// Precondition: 0 <= upto <= size().
    [[nodiscard]] constexpr isize find_last_set(isize upto) const { return _find_last<true>(upto); }
    [[nodiscard]] constexpr isize find_last_set() const { return _find_last<true>(_size()); }
    [[nodiscard]] constexpr isize find_last_unset(isize upto) const { return _find_last<false>(upto); }
    [[nodiscard]] constexpr isize find_last_unset() const { return _find_last<false>(_size()); }

    /// The indices of the set bits, ascending — one step per set bit, not per bit.
    ///
    ///     for (auto i : bs.set_indices())
    ///         ...
    ///
    /// The range borrows the bitset's storage, so it must not outlive it, and mutating the bitset while iterating
    /// leaves the cursor on a stale word.
    [[nodiscard]] constexpr cc::bit_index_range<WordT, true> set_indices() const { return {_words(), _size()}; }
    [[nodiscard]] constexpr cc::bit_index_range<WordT, false> unset_indices() const { return {_words(), _size()}; }

    // whole-set operations
public:
    /// Sets every bit that is set in `other` — the union.
    /// Precondition: other.size() == size().
    constexpr void set_all_of(ContainerT const& other)
    {
        _zip_with(other, [](WordT& w, WordT o) { w = WordT(w | o); });
    }

    /// Clears every bit that is set in `other` — the difference.
    /// Precondition: other.size() == size().
    constexpr void unset_all_of(ContainerT const& other)
    {
        _zip_with(other, [](WordT& w, WordT o) { w = WordT(w & WordT(~o)); });
    }

    /// Toggles every bit that is set in `other` — the symmetric difference.
    /// Precondition: other.size() == size().
    constexpr void toggle_all_of(ContainerT const& other)
    {
        _zip_with(other, [](WordT& w, WordT o) { w = WordT(w ^ o); });
    }

    /// Clears every bit that is NOT set in `other` — the intersection.
    /// Precondition: other.size() == size().
    constexpr void retain_all_of(ContainerT const& other)
    {
        _zip_with(other, [](WordT& w, WordT o) { w = WordT(w & o); });
    }

    /// Every bit set in `other` is set here too, i.e. other is a subset.
    /// Precondition: other.size() == size().
    [[nodiscard]] constexpr bool has_all(ContainerT const& other) const
    {
        _assert_same_size(other);
        auto const* const w = _words();
        auto const* const o = other._words_data();
        for (isize i = 0; i < _self().word_count(); ++i)
            if ((w[i] & o[i]) != o[i])
                return false;
        return true;
    }

    /// At least one bit is set in both.
    /// Precondition: other.size() == size().
    [[nodiscard]] constexpr bool has_any(ContainerT const& other) const
    {
        _assert_same_size(other);
        auto const* const w = _words();
        auto const* const o = other._words_data();
        for (isize i = 0; i < _self().word_count(); ++i)
            if ((w[i] & o[i]) != 0)
                return true;
        return false;
    }

    [[nodiscard]] constexpr bool is_disjoint(ContainerT const& other) const { return !has_any(other); }

    /// Number of bits set in both, without materializing the intersection.
    /// Precondition: other.size() == size().
    [[nodiscard]] constexpr isize intersection_bit_count(ContainerT const& other) const
    {
        _assert_same_size(other);
        isize count = 0;
        auto const* const w = _words();
        auto const* const o = other._words_data();
        for (isize i = 0; i < _self().word_count(); ++i)
            count += isize(cc::popcount(WordT(w[i] & o[i])));
        return count;
    }

    // factories for the non-mutating whole-set operations
public:
    /// Precondition: a.size() == b.size(); the result has that size.
    [[nodiscard]] static constexpr ContainerT create_union_of(ContainerT const& a, ContainerT const& b)
    {
        auto r = ContainerT(a);
        r.set_all_of(b);
        return r;
    }
    [[nodiscard]] static constexpr ContainerT create_intersection_of(ContainerT const& a, ContainerT const& b)
    {
        auto r = ContainerT(a);
        r.retain_all_of(b);
        return r;
    }
    /// The bits set in a but not in b.
    [[nodiscard]] static constexpr ContainerT create_difference_of(ContainerT const& a, ContainerT const& b)
    {
        auto r = ContainerT(a);
        r.unset_all_of(b);
        return r;
    }
    [[nodiscard]] static constexpr ContainerT create_symmetric_difference_of(ContainerT const& a, ContainerT const& b)
    {
        auto r = ContainerT(a);
        r.toggle_all_of(b);
        return r;
    }

    // implementation
protected:
    [[nodiscard]] constexpr ContainerT& _self() { return static_cast<ContainerT&>(*this); }
    [[nodiscard]] constexpr ContainerT const& _self() const { return static_cast<ContainerT const&>(*this); }

    [[nodiscard]] constexpr isize _size() const { return _self().size(); }
    [[nodiscard]] constexpr WordT* _words() { return _self()._words_data(); }
    [[nodiscard]] constexpr WordT const* _words() const { return _self()._words_data(); }

    [[nodiscard]] static constexpr isize _word_of(isize i) { return i / bits_per_word; }
    [[nodiscard]] static constexpr WordT _bit_of(isize i) { return WordT(u64(1) << (i % bits_per_word)); }

    /// Ones in [lo, hi) of a word; hi - lo must be in [0, bits_per_word].
    [[nodiscard]] static constexpr WordT _bits_in(isize lo, isize hi)
    {
        auto const width = hi - lo;
        auto const ones = width >= 64 ? ~u64(0) : (u64(1) << width) - 1;
        return WordT(ones << lo);
    }

    /// The bits of word `i` that belong to the bitset — all of them except in the last word, and none in a padding word.
    [[nodiscard]] constexpr WordT _valid_mask(isize i) const
    {
        auto const rest = _size() - i * bits_per_word;
        return rest >= bits_per_word ? all_ones : _bits_in(0, rest < 0 ? 0 : rest);
    }

    /// Restores the tail invariant after a member that wrote whole words.
    constexpr void _mask_tail()
    {
        auto const wc = _self().word_count();
        if (wc > 0)
        {
            auto& w = _words()[wc - 1];
            w = WordT(w & _valid_mask(wc - 1));
        }
    }

    constexpr void _assert_same_size(ContainerT const& other) const
    {
        CC_ASSERT(other.size() == _size(), "bitsets must have the same size; resize one of them first");
    }

    template <class F>
    constexpr void _zip_with(ContainerT const& other, F&& f)
    {
        _assert_same_size(other);
        auto* const w = _words();
        auto const* const o = other._words_data();
        for (isize i = 0; i < _self().word_count(); ++i)
            f(w[i], o[i]);
    }

    /// Calls f(word, mask) for each word the bit range [start, start + count) touches, masked to that range.
    template <class F>
    constexpr void _for_each_range_word(isize start, isize count, F&& f)
    {
        CC_ASSERT(0 <= start && 0 <= count && start + count <= _size(), "bit range out of bounds");
        if (count == 0)
            return;

        auto const last = start + count - 1;
        auto const first_word = _word_of(start);
        auto const last_word = _word_of(last);
        auto* const w = _words();

        for (auto i = first_word; i <= last_word; ++i)
        {
            auto const lo = i == first_word ? start % bits_per_word : 0;
            auto const hi = i == last_word ? last % bits_per_word + 1 : bits_per_word;
            f(w[i], _bits_in(lo, hi));
        }
    }

    template <bool Set>
    [[nodiscard]] constexpr WordT _load(isize i) const
    {
        if constexpr (Set)
            return _words()[i];
        else
            return WordT(WordT(~_words()[i]) & _valid_mask(i));
    }

    template <bool Set>
    [[nodiscard]] constexpr isize _find_first(isize from) const
    {
        CC_ASSERT(0 <= from && from <= _size(), "bit index out of bounds");
        if (from == _size())
            return -1;

        auto i = _word_of(from);
        auto w = WordT(_load<Set>(i) & _bits_in(from % bits_per_word, bits_per_word));
        while (true)
        {
            if (w != 0)
                return i * bits_per_word + isize(cc::count_trailing_zeroes(w));
            if (++i >= _self().word_count())
                return -1;
            w = _load<Set>(i);
        }
    }

    template <bool Set>
    [[nodiscard]] constexpr isize _find_last(isize upto) const
    {
        CC_ASSERT(0 <= upto && upto <= _size(), "bit index out of bounds");
        if (upto == 0)
            return -1;

        auto const last = upto - 1;
        auto i = _word_of(last);
        auto w = WordT(_load<Set>(i) & _bits_in(0, last % bits_per_word + 1));
        while (true)
        {
            if (w != 0)
                return i * bits_per_word + (bits_per_word - 1 - isize(cc::count_leading_zeroes(w)));
            if (i == 0)
                return -1;
            w = _load<Set>(--i);
        }
    }
};
