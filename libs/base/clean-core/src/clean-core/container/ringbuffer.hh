#pragma once

#include <clean-core/common/hash.hh> // cc::make_hash_range
#include <clean-core/common/macros.hh>
#include <clean-core/container/impl/ringbuffer_container.hh>
#include <clean-core/math/bit.hh> // cc::bit_ceil
#include <clean-core/memory/allocation.hh>

#include <initializer_list>
#include <type_traits>


// TODO:
// - adopting an existing cc::allocation, taking the largest power of two at or below its capacity
// - extract_allocation, which needs the content linearized (or a check that it already is)
// - equality, order
// - contains/find/count and the predicate-based removal family


/// Growable ring buffer: a queue over heap storage with strict O(1) push and pop at **both** ends.
///
/// The capacity is **always a power of two**, so the wrap-around is a mask rather than a modulo — a 64-bit division
/// would cost more than the rest of a push put together, and this type exists for the paths where that matters.
/// A requested capacity is therefore rounded up, and capacity() reports what you actually got.
///
/// Nothing here ever shifts an element, which buys a reference guarantee stronger than `cc::vector`'s:
/// a reference stays valid across pushes and pops at either end, and dies only when the ring grows.
/// The `_stable` members never grow, so a caller that stays inside the reserved capacity keeps every reference alive.
///
/// The elements are **not contiguous** once the content wraps, so there is no `data()` and no pointer iterator.
/// `segments()` hands out the up to two contiguous runs, and `linearize()` removes the wrap when one span is really needed.
///
/// Value semantics (deep copy); a moved-from ringbuffer is left empty.
/// Elements are constructed and destroyed in place, so `T` need not be default-constructible.
///
/// Storage is a `cc::allocation<T>` held as a **raw byte handle**: its live object range stays empty and the ring
/// manages element lifetimes itself, because a wrapped ring is not one contiguous window (see memory/allocation.hh).
/// That is also why the allocation-share protocol is absent here.
///
/// See [containers](../../../docs/containers.md) for the contracts every container shares.
///
/// Usage:
///   auto rb = cc::ringbuffer<int>::create_with_capacity(64); // 64 slots, no reallocation below that
///   rb.push_back(1);
///   auto oldest = rb.pop_front();
///   rb.push_back_overwriting(2); // a fixed-size window over the newest 64 values
template <class T>
struct cc::ringbuffer : private cc::ringbuffer_container<T, ringbuffer<T>, cc::isize>
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>,
                  "allocations need to refer to non-const objects, not references/functions/void");

    using base = cc::ringbuffer_container<T, ringbuffer<T>, cc::isize>;

    using typename base::const_iterator;
    using typename base::element_t;
    using typename base::iterator;

    // element access
public:
    using base::operator[]; // access element by index, 0 is the front
    using base::back;       // access newest element
    using base::front;      // access oldest element

    // iterators
public:
    using base::begin;     // iterate front to back
    using base::end;       //
    using base::linearize; // rearrange into one contiguous run and return it
    using base::segments;  // the up to two contiguous runs holding the content

    // queries
public:
    using base::empty;            // check if no elements are stored
    using base::free_capacity;    // elements that fit without growing
    using base::full;             // size() == capacity(), which still allows another (growing) push
    using base::has_capacity_for; // check if N more elements fit without growing
    using base::size;             // get number of elements
    using base::size_bytes;       // get total size in bytes

    /// Slots available without reallocation; always a power of two, and 0 before the first allocation.
    [[nodiscard]] constexpr isize capacity() const { return _mask + 1; }

    // insertion
public:
    using base::emplace_back;  // construct element at back (with allocation if needed)
    using base::emplace_front; // construct element at front (with allocation if needed)
    using base::push_back;     // add element at back (with allocation if needed)
    using base::push_front;    // add element at front (with allocation if needed)

    using base::emplace_back_stable;  // construct element at back (requires capacity)
    using base::emplace_front_stable; // construct element at front (requires capacity)
    using base::push_back_stable;     // add element at back (requires capacity)
    using base::push_front_stable;    // add element at front (requires capacity)

    using base::try_push_back;  // add element at back, false if full (never grows)
    using base::try_push_front; // add element at front, false if full (never grows)

    using base::push_back_overwriting;  // add element at back, dropping the front when full (never grows)
    using base::push_front_overwriting; // add element at front, dropping the back when full (never grows)

    using base::push_back_range;  // append a range at back (one reservation)
    using base::push_front_range; // prepend a contiguous range at front, keeping its order

    // removal
public:
    using base::pop_back;     // remove and return newest element
    using base::pop_front;    // remove and return oldest element
    using base::remove_back;  // remove newest element (no return value)
    using base::remove_front; // remove oldest element (no return value)

    using base::try_pop_back;  // remove and return newest element, nullopt if empty
    using base::try_pop_front; // remove and return oldest element, nullopt if empty

    using base::remove_back_n;  // remove the N newest elements
    using base::remove_front_n; // remove the N oldest elements

    using base::clear; // destroy all elements, size becomes 0

    // other mutations
public:
    using base::copy_to; // copy the elements front to back into a span
    using base::fill;    // fill all elements with value

    // capacity management
public:
    /// Ensures `count` elements fit without reallocation, rounding up to a power of two.
    /// Unlike a push, this does not apply a growth factor: reserve(5) gives exactly 8 slots.
    void reserve(isize count)
    {
        if (count > capacity())
            _reallocate_to(_pow2_at_least(count));
    }

    /// Reduces the capacity to the smallest power of two that holds the current elements.
    /// The content ends up linear at slot 0, and every reference into the ring is invalidated.
    void shrink_to_fit()
    {
        auto const tight = _pow2_at_least(this->size());
        if (tight < capacity())
            _reallocate_to(tight);
    }

    // factories
public:
    /// An empty ring with room for `capacity` elements, rounded up to a power of two.
    [[nodiscard]] static ringbuffer create_with_capacity(isize capacity, cc::memory_resource const* resource = nullptr)
    {
        CC_ASSERT(capacity >= 0, "capacity must be non-negative");
        auto rb = ringbuffer::create_with_resource(resource);
        rb.reserve(capacity);
        return rb;
    }

    /// An empty ring that will allocate from `resource`.
    [[nodiscard]] static ringbuffer create_with_resource(cc::memory_resource const* resource)
    {
        ringbuffer rb;
        rb._alloc.custom_resource = resource;
        return rb;
    }

    /// A deep copy of `source`, front to back, in a tight power-of-two capacity.
    [[nodiscard]] static ringbuffer create_copy_of(cc::span<T const> source, cc::memory_resource const* resource = nullptr)
    {
        auto rb = ringbuffer::create_with_capacity(source.size(), resource);
        rb.push_back_range(source);
        return rb;
    }

    // ctors / dtor / assignment
public:
    ringbuffer() = default;

    ringbuffer(std::initializer_list<T> init)
    {
        reserve(isize(init.size()));
        for (auto const& e : init)
            this->push_back_stable(e);
    }

    ~ringbuffer() { this->clear(); }

    ringbuffer(ringbuffer&& rhs) noexcept : _alloc(cc::move(rhs._alloc)), _mask(cc::exchange(rhs._mask, -1))
    {
        this->_start = cc::exchange(rhs._start, 0);
        this->_size = cc::exchange(rhs._size, 0);
    }
    ringbuffer& operator=(ringbuffer&& rhs) noexcept
    {
        if (this != &rhs)
        {
            this->clear();
            _alloc = cc::move(rhs._alloc);
            _mask = cc::exchange(rhs._mask, -1);
            this->_start = cc::exchange(rhs._start, 0);
            this->_size = cc::exchange(rhs._size, 0);
        }
        return *this;
    }

    ringbuffer(ringbuffer const& rhs)
    {
        _alloc.custom_resource = rhs._alloc.custom_resource;
        reserve(rhs.size());
        for (auto const& e : rhs)
            this->push_back_stable(e);
    }
    ringbuffer& operator=(ringbuffer const& rhs)
    {
        if (this != &rhs)
        {
            this->clear();
            reserve(rhs.size());
            for (auto const& e : rhs)
                this->push_back_stable(e);
        }
        return *this;
    }

    // hashing
public:
    /// Structural, order-dependent hash over the elements, front to back.
    /// Independent of capacity and of where the content happens to sit in the slot array.
    [[nodiscard]] friend u64 hash(ringbuffer const& rb) { return cc::make_hash_range(rb); }

    // implementation
private:
    friend base;

    [[nodiscard]] T* _slots() { return _alloc.obj_start; }
    [[nodiscard]] T const* _slots() const { return _alloc.obj_start; }

    /// Wraps an index from [0, 2 * capacity()) into the slot array.
    /// _mask is -1 while no allocation exists, where capacity() is 0 and no slot is ever addressed.
    [[nodiscard]] isize _wrap(isize idx) const { return idx & _mask; }

    void _ensure_capacity_for(isize count)
    {
        auto const needed = this->size() + count;
        if (needed > capacity()) [[unlikely]]
            _grow_for(needed);
    }

    /// Smallest power of two >= count, and 0 for count == 0.
    [[nodiscard]] static isize _pow2_at_least(isize count)
    {
        CC_ASSERT(count >= 0, "capacity must be non-negative");
        return count == 0 ? 0 : isize(cc::bit_ceil(u64(count)));
    }

    /// Growth policy: at least double, with the first allocation never below a cacheline's worth of elements.
    /// The cacheline floor is deliberately not applied to later growths, so an explicitly small capacity stays small.
    CC_COLD_FUNC void _grow_for(isize needed)
    {
        auto const first_alloc_floor = capacity() == 0 ? cc::max(isize(1), isize(64 / isize(sizeof(T)))) : isize(0);
        _reallocate_to(_pow2_at_least(cc::max({needed, capacity() * 2, first_alloc_floor})));
    }

    /// Moves the content into a fresh allocation of exactly `new_capacity` slots, leaving it linear at slot 0.
    /// new_capacity must be a power of two (or 0) and at least size().
    void _reallocate_to(isize new_capacity)
    {
        CC_ASSERT(new_capacity >= this->size(), "cannot reallocate below the live element count");
        CC_ASSERT(new_capacity == 0 || cc::has_single_bit(u64(new_capacity)), "capacity must be a power of two");

        auto const bytes = new_capacity * isize(sizeof(T));
        auto new_alloc = cc::allocation<T>::create_empty_bytes(bytes, bytes, alignof(T), _alloc.custom_resource);

        // The new allocation keeps an empty live range: `cursor` is ours, so obj_end never moves.
        auto* cursor = new_alloc.obj_start;
        auto const [a, b] = this->segments();
        cc::impl::move_create_objects_to(cursor, a.begin(), a.end());
        cc::impl::move_create_objects_to(cursor, b.begin(), b.end());
        cc::impl::destroy_objects_in_reverse(b.begin(), b.end());
        cc::impl::destroy_objects_in_reverse(a.begin(), a.end());

        _alloc = cc::move(new_alloc); // frees the old bytes
        _mask = new_capacity - 1;
        this->_start = 0;
    }

    /// The owning byte block, deliberately holding no live objects of its own.
    cc::allocation<T> _alloc;

    /// capacity() - 1, so the wrap is a single AND; -1 while no allocation exists.
    isize _mask = -1;
};
