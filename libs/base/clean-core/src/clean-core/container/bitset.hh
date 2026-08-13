#pragma once

#include <clean-core/common/hash.hh>    // cc::make_hash, cc::make_hash_range
#include <clean-core/common/utility.hh> // cc::move, cc::exchange
#include <clean-core/container/impl/bitset_container.hh>
#include <clean-core/memory/allocation.hh>


// TODO:
// - to_string / to_debug_string, printing index 0 LEFTMOST (a bitset is an indexed bit array, not a number)
// - adopting and extracting the underlying cc::allocation<u64>
// - the mixed-size whole-set operations, if a caller ever really wants zero-extension


/// Dynamically sized bit set: a runtime number of bits packed into heap-allocated u64 words.
///
/// Value semantics (deep copy); a moved-from bitset is left empty.
/// Storage is a `cc::allocation<u64>` whose live object range is the whole word capacity, since a word is trivial and
/// the spare words carry the invariant below rather than being uninitialized.
///
/// A bit set is not a container of addressable bools: `operator[]` hands out a cc::bit_ref proxy, uniformly, and
/// there is no begin()/end() over bools.
/// `set_indices()` is the iteration worth having — it steps once per set bit rather than once per bit.
///
/// There is deliberately no `empty()`: it would sit one letter from `none_set()` while meaning something else entirely.
/// `clear()` keeps its container meaning, so it makes size() zero; zeroing the bits in place is `unset_all()`.
///
/// The whole-set operations require an equal size() and assert otherwise.
/// Zero-extending a shorter operand would truncate under retain_all_of while growing nothing under set_all_of, and
/// that asymmetry hides an off-by-one instead of reporting it — resize first where a mixed size is really intended.
///
/// See [containers](../../../docs/containers.md) for the contracts every container shares.
///
/// Usage:
///   auto visited = cc::bitset::create_defaulted(node_count);
///   visited.set(n);
///   for (auto i : visited.set_indices())
///       ...
struct cc::bitset : cc::bitset_container<bitset, u64>
{
    using base = cc::bitset_container<bitset, u64>;
    using base::bits_per_word;
    using typename base::word_t;

    // queries
public:
    /// Number of bits.
    [[nodiscard]] constexpr isize size() const { return _size; }

    /// Words covering those bits, i.e. ceil(size() / 64).
    [[nodiscard]] constexpr isize word_count() const { return (_size + bits_per_word - 1) / bits_per_word; }

    /// Bits that fit without reallocation; always a whole number of words.
    [[nodiscard]] constexpr isize capacity() const { return _word_capacity() * bits_per_word; }

    using base::operator[]; // access a bit by index, as bool (const) or a cc::bit_ref proxy
    using base::is_set;     // check whether a single bit is set
    using base::words_span; // the raw words

    using base::all_set;         // every bit is set (vacuously true at size 0)
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
    using base::unset_all;  // clear every bit, WITHOUT changing size()

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

    // growth
public:
    /// Appends one bit, growing exponentially when the capacity runs out.
    void push_back(bool value)
    {
        auto const needed = (_size + 1 + bits_per_word - 1) / bits_per_word;
        if (needed > _word_capacity())
        {
            auto const grown = _word_capacity() * 2;
            _reserve_words(needed > grown ? needed : grown);
        }
        ++_size;
        if (value)
            set(_size - 1);
    }

    /// Removes the last bit and returns it.
    /// Precondition: size() > 0.
    [[nodiscard]] bool pop_back()
    {
        CC_ASSERT(_size > 0, "pop_back on an empty bitset");
        auto const value = is_set(_size - 1);
        unset(_size - 1);
        --_size;
        return value;
    }

    /// Removes the last bit without returning it.
    /// Precondition: size() > 0.
    void remove_back()
    {
        CC_ASSERT(_size > 0, "remove_back on an empty bitset");
        unset(_size - 1);
        --_size;
    }

    /// Resizes to `new_size` bits, filling the new ones with `value`.
    /// Shrinking keeps the surviving bits; the bits it dropped do not come back when the bitset grows again.
    void resize_to_filled(isize new_size, bool value = false)
    {
        CC_ASSERT(new_size >= 0, "bitset size must be non-negative");
        if (new_size < _size)
        {
            unset_range(new_size, _size - new_size); // keeps every word above the new tail zero
            _size = new_size;
            return;
        }
        if (new_size == _size)
            return;

        _reserve_words((new_size + bits_per_word - 1) / bits_per_word);
        auto const old_size = _size;
        _size = new_size;
        if (value)
            set_range(old_size, new_size - old_size);
    }

    /// Ensures `bit_count` bits fit without reallocation; allocates exactly, rounded up to a word.
    void reserve(isize bit_count)
    {
        CC_ASSERT(bit_count >= 0, "bitset capacity must be non-negative");
        _reserve_words((bit_count + bits_per_word - 1) / bits_per_word);
    }

    /// Reduces the capacity to the words actually covering size().
    void shrink_to_fit()
    {
        if (word_count() == _word_capacity())
            return;
        if (word_count() == 0)
        {
            _alloc = {};
            return;
        }
        _set_word_capacity(word_count());
    }

    /// Drops every bit, so size() becomes 0; the capacity survives.
    void clear()
    {
        unset_all();
        _size = 0;
    }

    // factories
public:
    using base::create_difference_of;           // the bits set in a but not in b
    using base::create_intersection_of;         //
    using base::create_symmetric_difference_of; //
    using base::create_union_of;                //

    /// `bit_count` bits, all unset.
    [[nodiscard]] static bitset create_defaulted(isize bit_count, cc::memory_resource const* resource = nullptr)
    {
        return create_filled(bit_count, false, resource);
    }

    /// `bit_count` bits, all set to `value`.
    [[nodiscard]] static bitset create_filled(isize bit_count, bool value, cc::memory_resource const* resource = nullptr)
    {
        CC_ASSERT(bit_count >= 0, "bitset size must be non-negative");
        auto r = create_with_resource(resource);
        r.resize_to_filled(bit_count, value);
        return r;
    }

    /// No bits, but storage for `bit_count` of them.
    [[nodiscard]] static bitset create_with_capacity(isize bit_count, cc::memory_resource const* resource = nullptr)
    {
        auto r = create_with_resource(resource);
        r.reserve(bit_count);
        return r;
    }

    /// Empty, and allocating from `resource` from here on.
    [[nodiscard]] static bitset create_with_resource(cc::memory_resource const* resource)
    {
        auto r = bitset();
        r._alloc.custom_resource = resource;
        return r;
    }

    // ctors / dtor / assignment
public:
    bitset() = default;
    ~bitset() = default;

    bitset(bitset const& rhs) : _alloc(cc::allocation<u64>::create_copy_of(rhs._alloc)), _size(rhs._size) {}

    bitset& operator=(bitset const& rhs)
    {
        if (this != &rhs)
        {
            clear(); // zeroes the capacity, so the words above rhs's tail keep the invariant
            _reserve_words(rhs.word_count());
            for (isize i = 0; i < rhs.word_count(); ++i)
                _words_data()[i] = rhs._words_data()[i];
            _size = rhs._size;
        }
        return *this;
    }

    bitset(bitset&& rhs) noexcept : _alloc(cc::move(rhs._alloc)), _size(cc::exchange(rhs._size, 0)) {}

    bitset& operator=(bitset&& rhs) noexcept
    {
        if (this != &rhs)
        {
            _alloc = cc::move(rhs._alloc);
            _size = cc::exchange(rhs._size, 0);
        }
        return *this;
    }

    // comparison / hashing
public:
    /// Two bitsets are equal iff they have the same size and the same bits set.
    [[nodiscard]] friend bool operator==(bitset const& a, bitset const& b)
    {
        if (a._size != b._size)
            return false;
        for (isize i = 0; i < a.word_count(); ++i)
            if (a._words_data()[i] != b._words_data()[i])
                return false;
        return true;
    }

    [[nodiscard]] friend u64 hash(bitset const& bs)
    {
        return cc::make_hash(bs._size, cc::make_hash_range(bs.words_span()));
    }

    // implementation
private:
    friend base;

    [[nodiscard]] constexpr u64* _words_data() { return _alloc.obj_start; }
    [[nodiscard]] constexpr u64 const* _words_data() const { return _alloc.obj_start; }

    /// Words the allocation holds; every one at or above word_count() is zero, which is what lets growth skip zeroing.
    [[nodiscard]] constexpr isize _word_capacity() const { return _alloc.obj_end - _alloc.obj_start; }

    void _reserve_words(isize new_word_count)
    {
        if (new_word_count <= _word_capacity())
            return;
        _set_word_capacity(new_word_count);
    }

    /// Moves the content into a fresh allocation of exactly `new_word_count` words, zeroing the ones past word_count().
    /// Precondition: new_word_count >= word_count().
    void _set_word_capacity(isize new_word_count)
    {
        CC_ASSERT(new_word_count >= word_count(), "cannot drop words that still hold bits");

        auto new_alloc = cc::allocation<u64>::create_uninitialized(new_word_count, _alloc.custom_resource);
        auto* const dst = new_alloc.obj_start;
        for (isize i = 0; i < word_count(); ++i)
            dst[i] = _words_data()[i];
        for (isize i = word_count(); i < new_word_count; ++i)
            dst[i] = 0;

        _alloc = cc::move(new_alloc);
    }

    cc::allocation<u64> _alloc;
    isize _size = 0;
};
