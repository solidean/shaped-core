#pragma once

#include <clean-core/common/hash.hh> // cc::make_hash_range
#include <clean-core/common/impl/small_size_type.hh>
#include <clean-core/container/impl/ringbuffer_container.hh>

#include <initializer_list>
#include <type_traits>


// TODO:
// - equality, order
// - contains/find/count and the predicate-based removal family


/// Fixed-capacity ring buffer: a queue over `N` inline slots with strict O(1) push and pop at **both** ends,
/// and **no dynamic allocation ever**.
///
/// `N` is a hard cap rather than a hint: pushing into a full ring asserts instead of growing.
/// Where an overflow should silently drop the oldest element instead, that is what `push_back_overwriting` is for,
/// and it makes this the natural "newest N samples" window — a frame-time history, a scroll-back, a rolling average.
///
/// Any `N` is fine, powers of two included but not required.
/// The wrap costs one compare-and-subtract rather than a division, because `_start` and the index are both below `N`.
/// `N == 0` is a valid, permanently full and permanently empty ring where every push asserts.
///
/// Nothing here ever shifts an element, so a reference stays valid until that element is removed — there is no
/// reallocation to invalidate it, and no `_stable` family, since every operation already is.
///
/// The elements are **not contiguous** once the content wraps, so there is no `data()` and no pointer iterator.
/// `segments()` hands out the up to two contiguous runs, and `linearize()` removes the wrap when one span is really needed.
///
/// Value semantics (deep copy); a moved-from fixed_ringbuffer is left empty.
/// Elements are constructed and destroyed in place, so `T` need not be default-constructible.
///
/// See [containers](../../../docs/containers.md) for the contracts every container shares.
///
/// Usage:
///   cc::fixed_ringbuffer<float, 128> history; // never allocates
///   history.push_back_overwriting(dt);        // keeps the newest 128 frame times
template <class T, cc::isize N>
struct cc::fixed_ringbuffer : private cc::ringbuffer_container<T, fixed_ringbuffer<T, N>, cc::impl::small_size_t<u64(N)>>
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>, "fixed_ringbuffer needs non-const object elements");
    static_assert(N >= 0, "fixed_ringbuffer capacity N must be non-negative");

    using base = cc::ringbuffer_container<T, fixed_ringbuffer<T, N>, cc::impl::small_size_t<u64(N)>>;

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
    using base::free_capacity;    // elements that still fit
    using base::full;             // size() == N, where any further push asserts
    using base::has_capacity_for; // check if N more elements still fit
    using base::size;             // get number of elements
    using base::size_bytes;       // get total size in bytes

    /// The compile-time capacity — the hard cap on element count.
    [[nodiscard]] static constexpr isize capacity() { return N; }

    // insertion — no `_stable` variants: a fixed_ringbuffer never reallocates, so every insertion already is stable.
public:
    using base::emplace_back;  // construct element at back (asserts if full)
    using base::emplace_front; // construct element at front (asserts if full)
    using base::push_back;     // add element at back (asserts if full)
    using base::push_front;    // add element at front (asserts if full)

    using base::try_push_back;  // add element at back, false if full
    using base::try_push_front; // add element at front, false if full

    using base::push_back_overwriting;  // add element at back, dropping the front when full
    using base::push_front_overwriting; // add element at front, dropping the back when full

    using base::push_back_range;  // append a range at back
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

    // factories
public:
    /// A deep copy of `source`, front to back.
    /// Precondition: source.size() <= N.
    [[nodiscard]] static fixed_ringbuffer create_copy_of(cc::span<T const> source)
    {
        fixed_ringbuffer rb;
        rb.push_back_range(source);
        return rb;
    }

    // ctors / dtor / assignment
public:
    // User-provided rather than `= default`, so a const fixed_ringbuffer is const-default-constructible.
    // Leaves the storage uninitialized; the counters start at 0 through their own default member initializers.
    fixed_ringbuffer() {}

    fixed_ringbuffer(std::initializer_list<T> init)
    {
        CC_ASSERT(isize(init.size()) <= N, "fixed_ringbuffer initializer exceeds capacity");
        for (auto const& e : init)
            this->push_back(e);
    }

    ~fixed_ringbuffer() { this->clear(); }

    fixed_ringbuffer(fixed_ringbuffer const& rhs)
    {
        for (auto const& e : rhs)
            this->push_back(e);
    }
    fixed_ringbuffer(fixed_ringbuffer&& rhs) noexcept
    {
        for (auto& e : rhs)
            this->emplace_back(cc::move(e));
        rhs.clear();
    }
    fixed_ringbuffer& operator=(fixed_ringbuffer const& rhs)
    {
        if (this != &rhs)
        {
            this->clear();
            for (auto const& e : rhs)
                this->push_back(e);
        }
        return *this;
    }
    fixed_ringbuffer& operator=(fixed_ringbuffer&& rhs) noexcept
    {
        if (this != &rhs)
        {
            this->clear();
            for (auto& e : rhs)
                this->emplace_back(cc::move(e));
            rhs.clear();
        }
        return *this;
    }

    // hashing
public:
    /// Structural, order-dependent hash over the elements, front to back.
    /// Independent of where the content happens to sit in the slot array.
    [[nodiscard]] friend u64 hash(fixed_ringbuffer const& rb) { return cc::make_hash_range(rb); }

    // implementation
private:
    friend base;

    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    /// The reinterpret_cast is well-defined for objects placement-new'd into this buffer.
    [[nodiscard]] T* _slots() { return reinterpret_cast<T*>(_storage); }
    [[nodiscard]] T const* _slots() const { return reinterpret_cast<T const*>(_storage); }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

    /// Wraps an index from [0, 2 * N) into the slot array.
    /// One compare-and-subtract, so an arbitrary N never costs a division.
    [[nodiscard]] static constexpr isize _wrap(isize idx) { return idx >= N ? idx - N : idx; }

    void _ensure_capacity_for(isize count)
    {
        CC_ASSERT(this->size() + count <= N, "fixed_ringbuffer capacity exceeded");
    }

    // Uninitialized aligned storage for N slots; only the live ones hold objects.
    // Sized to at least 1 byte so N == 0 does not form a zero-length array, which would be UB.
    alignas(T) unsigned char _storage[N == 0 ? 1 : sizeof(T) * N];
};
