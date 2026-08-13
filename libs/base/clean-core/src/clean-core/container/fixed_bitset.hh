#pragma once

#include <clean-core/common/hash.hh> // cc::make_hash_range
#include <clean-core/container/impl/bitset_container.hh>

/// Fixed-size bit set: `N` bits packed into inline words, with **no dynamic allocation ever**.
///
/// The word type is the smallest that covers `N` — `fixed_bitset<8>` is one byte, `fixed_bitset<32>` four, and
/// anything past 64 bits an array of u64.
/// A default-constructed fixed_bitset is all zeros, unlike the inline containers whose storage starts uninitialized.
///
/// Everything here is constexpr, and the type is **structural**: it can serve as a non-type template parameter,
/// which is why `words` is public.
/// Prefer the named operations; the raw words are for interop and for compile-time tables.
///
/// `N == 0` is a valid, permanently empty bit set.
///
/// A bit set is not a container of addressable bools: `operator[]` hands out a cc::bit_ref proxy, uniformly, and
/// there is no begin()/end() over bools.
/// `set_indices()` is the iteration worth having — it steps once per set bit rather than once per bit.
///
/// There is deliberately no `empty()`: it would sit one letter from `none_set()` while meaning something else entirely.
/// The whole-set operations require an equal size, which for two fixed_bitsets means they simply do not compile
/// against a different `N`.
///
/// See [containers](../../../docs/containers.md) for the contracts every container shares.
///
/// Usage:
///   auto occupied = cc::fixed_bitset<64>();     // never allocates, all bits zero
///   occupied.set(3);
///   auto const free_slot = occupied.find_first_unset();
template <cc::isize N>
struct cc::fixed_bitset : cc::bitset_container<fixed_bitset<N>, cc::impl::bitset_word_t<u64(N)>>
{
    static_assert(N >= 0, "fixed_bitset size N must be non-negative");

    using base = cc::bitset_container<fixed_bitset<N>, cc::impl::bitset_word_t<u64(N)>>;
    using base::bits_per_word;
    using typename base::word_t;

    /// The words backing the bits, lowest index in the least significant bit of word 0.
    ///
    /// Public because that is what makes fixed_bitset a structural type, so a bit set can serve as a non-type
    /// template parameter — the same trade cc::flags makes for its `bits`.
    ///
    /// The initializer is load-bearing: it is what makes a default-constructed fixed_bitset all-zero rather than
    /// indeterminate, and it keeps the type trivially copyable.
    ///
    /// N == 0 still holds one word, since a zero-length array would be UB; word_count() reports 0 and nothing reads it.
    word_t words[N == 0 ? 1 : (N + bits_per_word - 1) / bits_per_word] = {};

    // queries
public:
    /// The compile-time bit count.
    [[nodiscard]] static constexpr isize size() { return N; }

    /// Words actually covering those bits, so 0 for N == 0.
    [[nodiscard]] static constexpr isize word_count() { return (N + bits_per_word - 1) / bits_per_word; }

    /// The storage hook cc::bitset_container drives; `words` is the public spelling.
    [[nodiscard]] constexpr word_t* _words_data() { return words; }
    [[nodiscard]] constexpr word_t const* _words_data() const { return words; }

    using base::operator[]; // access a bit by index, as bool (const) or a cc::bit_ref proxy
    using base::is_set;     // check whether a single bit is set
    using base::words_span; // the raw words

    using base::all_set;         // every bit is set (vacuously true for N == 0)
    using base::any_set;         // at least one bit is set
    using base::none_set;        // no bit is set
    using base::set_bit_count;   // number of set bits
    using base::unset_bit_count; // number of unset bits

    // mutation
public:
    using base::set;    // set a bit, or set(i, on) to assign one
    using base::toggle; // flip a bit
    using base::unset;  // clear a bit

    using base::set_all;    // set every bit, or set_all(on) to assign them all
    using base::toggle_all; // flip every bit
    using base::unset_all;  // clear every bit

    using base::set_range;    // set the bits in [start, start + count)
    using base::toggle_range; // flip that range
    using base::unset_range;  // clear that range

    // search
public:
    using base::find_first_set;   // lowest set bit at or above `from`, -1 if none
    using base::find_first_unset; //
    using base::find_last_set;    // highest set bit below `upto`, -1 if none
    using base::find_last_unset;  //

    using base::set_indices;   // the indices of the set bits, ascending
    using base::unset_indices; //

    // whole-set operations
public:
    using base::retain_all_of; // intersection: clear every bit not set in the other
    using base::set_all_of;    // union: set every bit set in the other
    using base::toggle_all_of; // symmetric difference
    using base::unset_all_of;  // difference: clear every bit set in the other

    using base::has_all;                // the other is a subset of this
    using base::has_any;                // at least one bit is set in both
    using base::intersection_bit_count; // popcount of the intersection, nothing materialized
    using base::is_disjoint;            // no bit is set in both

    // factories
public:
    using base::create_difference_of;           // the bits set in a but not in b
    using base::create_intersection_of;         //
    using base::create_symmetric_difference_of; //
    using base::create_union_of;                //

    [[nodiscard]] static constexpr fixed_bitset create_all_set()
    {
        auto r = fixed_bitset();
        r.set_all();
        return r;
    }

    /// The low N bits of `bits`, index 0 taking the least significant one.
    /// Bits at or above N must be zero.
    [[nodiscard]] static constexpr fixed_bitset create_from_u64(u64 bits)
        requires(N <= 64)
    {
        CC_ASSERT(N == 64 || bits < (u64(1) << (N < 64 ? N : 0)), "bits do not fit into a fixed_bitset of this size");
        auto r = fixed_bitset();
        if constexpr (N > 0)
            r.words[0] = word_t(bits);
        return r;
    }

    /// The bits as an integer, index 0 in the least significant bit.
    [[nodiscard]] constexpr u64 to_u64() const
        requires(N <= 64)
    {
        if constexpr (N == 0)
            return 0;
        else
            return u64(words[0]);
    }

    // comparison / hashing
public:
    /// Two fixed_bitsets are equal iff the same bits are set, which the tail invariant makes a plain word compare.
    /// Hand-written rather than defaulted: a defaulted comparison would have to compare the (comparison-less) mixin base.
    [[nodiscard]] friend constexpr bool operator==(fixed_bitset const& a, fixed_bitset const& b)
    {
        for (isize i = 0; i < word_count(); ++i)
            if (a.words[i] != b.words[i])
                return false;
        return true;
    }

    [[nodiscard]] friend constexpr u64 hash(fixed_bitset const& bs) { return cc::make_hash_range(bs.words_span()); }
};
