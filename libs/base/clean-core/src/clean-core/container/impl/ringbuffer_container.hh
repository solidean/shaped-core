#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/pair.hh>
#include <clean-core/container/span.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/impl/object_lifetime_util.hh>

// The shared surface of cc::ringbuffer and cc::fixed_ringbuffer, plus the iterator both hand out.
//
// The ring stores the slot of the front element and the element count, rather than two monotonic counters:
// size() is then free, and "full" never aliases "empty" the way equal counters would.
// Every access goes through _slot(i), which adds the logical index to the start and asks the derived type to wrap it.
//
// The derived type owns the storage and supplies, privately:
//   T* _slots() / T const* _slots() const      base of the slot array
//   isize capacity() const                     number of slots (public on both containers)
//   isize _wrap(isize idx) const               idx in [0, 2 * capacity()) -> idx mod capacity()
//   void _ensure_capacity_for(isize count)     grows, or asserts where growth is impossible
//
// That split is what keeps the wrap free of a division: cc::ringbuffer masks against a power-of-two capacity,
// cc::fixed_ringbuffer subtracts N once, and neither ever divides.

/// Random-access iterator over a ring buffer, handed out by ringbuffer::begin() / end().
///
/// `RingT` is the ring type and carries the constness of the iteration, so `T` is `T const` for a const ring.
/// Dereferencing goes through the ring's operator[], which re-derives the slot — an iterator therefore survives
/// pushes and pops at the *other* end, but not a growth that moves the storage.
template <class T, class RingT>
struct cc::ringbuffer_iterator
{
    constexpr ringbuffer_iterator() = default;
    constexpr ringbuffer_iterator(RingT* ring, isize idx) : _ring(ring), _idx(idx) {}

    [[nodiscard]] constexpr T& operator*() const { return (*_ring)[_idx]; }
    [[nodiscard]] constexpr T* operator->() const { return &(*_ring)[_idx]; }
    [[nodiscard]] constexpr T& operator[](isize n) const { return (*_ring)[_idx + n]; }

    constexpr ringbuffer_iterator& operator++()
    {
        ++_idx;
        return *this;
    }
    constexpr ringbuffer_iterator operator++(int)
    {
        auto const copy = *this;
        ++_idx;
        return copy;
    }
    constexpr ringbuffer_iterator& operator--()
    {
        --_idx;
        return *this;
    }
    constexpr ringbuffer_iterator operator--(int)
    {
        auto const copy = *this;
        --_idx;
        return copy;
    }
    constexpr ringbuffer_iterator& operator+=(isize n)
    {
        _idx += n;
        return *this;
    }
    constexpr ringbuffer_iterator& operator-=(isize n)
    {
        _idx -= n;
        return *this;
    }

    [[nodiscard]] friend constexpr ringbuffer_iterator operator+(ringbuffer_iterator it, isize n) { return it += n; }
    [[nodiscard]] friend constexpr ringbuffer_iterator operator+(isize n, ringbuffer_iterator it) { return it += n; }
    [[nodiscard]] friend constexpr ringbuffer_iterator operator-(ringbuffer_iterator it, isize n) { return it -= n; }

    /// Distance in elements; both iterators must come from the same ring.
    [[nodiscard]] friend constexpr isize operator-(ringbuffer_iterator const& a, ringbuffer_iterator const& b)
    {
        return a._idx - b._idx;
    }

    [[nodiscard]] friend constexpr bool operator==(ringbuffer_iterator const& a, ringbuffer_iterator const& b)
    {
        return a._idx == b._idx;
    }
    [[nodiscard]] friend constexpr bool operator!=(ringbuffer_iterator const& a, ringbuffer_iterator const& b)
    {
        return a._idx != b._idx;
    }
    [[nodiscard]] friend constexpr bool operator<(ringbuffer_iterator const& a, ringbuffer_iterator const& b)
    {
        return a._idx < b._idx;
    }
    [[nodiscard]] friend constexpr bool operator<=(ringbuffer_iterator const& a, ringbuffer_iterator const& b)
    {
        return a._idx <= b._idx;
    }
    [[nodiscard]] friend constexpr bool operator>(ringbuffer_iterator const& a, ringbuffer_iterator const& b)
    {
        return a._idx > b._idx;
    }
    [[nodiscard]] friend constexpr bool operator>=(ringbuffer_iterator const& a, ringbuffer_iterator const& b)
    {
        return a._idx >= b._idx;
    }

private:
    RingT* _ring = nullptr;
    isize _idx = 0;
};

/// Mixin implementing the common "queue over a wrap-around slot array" surface area.
///
/// `IndexT` is the storage type of the two counters — `isize` for the heap ring, a packed unsigned type for the inline one.
/// Both counters are converted at the public boundary, so the unsigned storage never leaks into the signed API.
///
/// Index 0 is the front, and every runtime index is CC_ASSERT-checked.
/// Nothing here ever shifts an element, which is the guarantee the whole type exists for.
template <class T, class ContainerT, class IndexT>
struct cc::ringbuffer_container
{
    using element_t = T;
    using iterator = cc::ringbuffer_iterator<T, ContainerT>;
    using const_iterator = cc::ringbuffer_iterator<T const, ContainerT const>;

    // element access
public:
    /// Returns a reference to the i-th element counted from the front.
    /// Precondition: 0 <= i < size().
    [[nodiscard]] constexpr T& operator[](isize i)
    {
        CC_ASSERT(0 <= i && i < size(), "index out of bounds");
        return *_slot(i);
    }
    [[nodiscard]] constexpr T const& operator[](isize i) const
    {
        CC_ASSERT(0 <= i && i < size(), "index out of bounds");
        return *_slot(i);
    }

    /// Returns a reference to the oldest element, the one pop_front() would return.
    /// Precondition: !empty().
    [[nodiscard]] constexpr T& front()
    {
        CC_ASSERT(!empty(), "front() on empty ringbuffer");
        return *_slot(0);
    }
    [[nodiscard]] constexpr T const& front() const
    {
        CC_ASSERT(!empty(), "front() on empty ringbuffer");
        return *_slot(0);
    }

    /// Returns a reference to the newest element, the one pop_back() would return.
    /// Precondition: !empty().
    [[nodiscard]] constexpr T& back()
    {
        CC_ASSERT(!empty(), "back() on empty ringbuffer");
        return *_slot(size() - 1);
    }
    [[nodiscard]] constexpr T const& back() const
    {
        CC_ASSERT(!empty(), "back() on empty ringbuffer");
        return *_slot(size() - 1);
    }

    // iterators
public:
    /// Iterates front to back.
    /// There is no data() and no pointer iterator: the elements are not contiguous unless linearize() made them so.
    [[nodiscard]] constexpr iterator begin() { return iterator(&_self(), 0); }
    [[nodiscard]] constexpr iterator end() { return iterator(&_self(), size()); }
    [[nodiscard]] constexpr const_iterator begin() const { return const_iterator(&_self(), 0); }
    [[nodiscard]] constexpr const_iterator end() const { return const_iterator(&_self(), size()); }

    /// The live elements as the up to two contiguous runs they occupy, front run first.
    /// The second run is empty unless the content wraps, and both are empty for an empty ring.
    /// This is the seam for bulk memcpy work — writing through it must not change how many elements are live.
    [[nodiscard]] cc::pair<cc::span<T>, cc::span<T>> segments()
    {
        auto* const slots = _self()._slots();
        auto const first = _first_run_length();
        return {cc::span<T>(slots + isize(_start), first), cc::span<T>(slots, size() - first)};
    }
    [[nodiscard]] cc::pair<cc::span<T const>, cc::span<T const>> segments() const
    {
        auto const* const slots = _self()._slots();
        auto const first = _first_run_length();
        return {cc::span<T const>(slots + isize(_start), first), cc::span<T const>(slots, size() - first)};
    }

    /// Rearranges the elements so they occupy one contiguous run, and returns it.
    /// A ring whose content does not wrap is already linear and is left exactly where it is, rather than moved to slot 0.
    ///
    /// Every live element is move-constructed at most once, and every reference into a moved element dies.
    /// T must be move-constructible.
    cc::span<T> linearize()
    {
        auto const cap = _self().capacity();
        auto const count = size();
        auto* const slots = _self()._slots();

        if (count == 0)
        {
            _start = 0;
            return cc::span<T>(slots, isize(0));
        }

        auto const start = isize(_start);
        if (start + count <= cap)
            return cc::span<T>(slots + start, count);

        // Unwrapping while keeping the order is exactly "rotate the whole slot array left by _start":
        // slot j ends up holding what slot (j + _start) mod capacity() held, so the content lands at [0, count).
        // Shifting only the wrapped run would be cheaper but wrong — it would put the tail run behind the head run.
        //
        // The rotation is walked as permutation cycles, so a live element is moved exactly once.
        // Dead slots are not skipped but carried: the hole travels along the cycle like any other value.
        alignas(T) unsigned char hold_storage[sizeof(T)];
        auto* const hold = reinterpret_cast<T*>(hold_storage); // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)

        for (isize cycle_start = 0, processed = 0; processed < cap; ++cycle_start)
        {
            auto const holding = _slot_was_live(cycle_start, start, count, cap);
            if (holding)
            {
                new (cc::placement_new, hold) T(cc::move(slots[cycle_start]));
                slots[cycle_start].~T();
            }

            auto dst = cycle_start;
            while (true)
            {
                auto const src = _self()._wrap(dst + start);
                ++processed;
                if (src == cycle_start)
                    break;

                if (_slot_was_live(src, start, count, cap))
                {
                    new (cc::placement_new, slots + dst) T(cc::move(slots[src]));
                    slots[src].~T();
                }
                dst = src;
            }

            if (holding)
            {
                new (cc::placement_new, slots + dst) T(cc::move(*hold));
                hold->~T();
            }
        }

        _start = 0;
        return cc::span<T>(slots, count);
    }

    // queries
public:
    /// Number of live elements.
    [[nodiscard]] constexpr isize size() const { return isize(_size); }
    /// Total size in bytes of the live elements.
    [[nodiscard]] constexpr isize size_bytes() const { return size() * isize(sizeof(T)); }
    /// True if size() == 0.
    [[nodiscard]] constexpr bool empty() const { return _size == 0; }
    /// True if size() == capacity(), which for a growable ring still allows another push_back.
    [[nodiscard]] constexpr bool full() const { return size() == _self().capacity(); }
    /// Elements that fit without growing; a ring has one free count, not a separate one per end.
    [[nodiscard]] constexpr isize free_capacity() const { return _self().capacity() - size(); }
    [[nodiscard]] constexpr bool has_capacity_for(isize count) const { return free_capacity() >= count; }

    // insertion using existing capacity
public:
    /// Constructs a new element at the back using existing capacity.
    /// Never grows and never moves an element, so every reference into the ring stays valid.
    /// Precondition: !full().
    template <class... Args>
    T& emplace_back_stable(Args&&... args)
    {
        CC_ASSERT(!full(), "ringbuffer is full");
        auto* const p = _self()._slots() + _self()._wrap(isize(_start) + size());
        new (cc::placement_new, p) T(cc::forward<Args>(args)...);
        _size = IndexT(size() + 1); // _after_ so exceptions in T(...) leave the state valid
        return *p;
    }
    /// Copy-constructs a new element at the back using existing capacity.
    /// Precondition: !full().
    T& push_back_stable(T const& value) { return emplace_back_stable(value); }
    /// Move-constructs a new element at the back using existing capacity.
    /// Precondition: !full().
    T& push_back_stable(T&& value) { return emplace_back_stable(cc::move(value)); }

    /// Constructs a new element at the front using existing capacity.
    /// Never grows and never moves an element, so every reference into the ring stays valid.
    /// Precondition: !full().
    template <class... Args>
    T& emplace_front_stable(Args&&... args)
    {
        CC_ASSERT(!full(), "ringbuffer is full");
        auto const cap = _self().capacity();
        auto const slot = _self()._wrap(isize(_start) + cap - 1);
        auto* const p = _self()._slots() + slot;
        new (cc::placement_new, p) T(cc::forward<Args>(args)...);
        _start = IndexT(slot); // both _after_ so exceptions in T(...) leave the state valid
        _size = IndexT(size() + 1);
        return *p;
    }
    /// Copy-constructs a new element at the front using existing capacity.
    /// Precondition: !full().
    T& push_front_stable(T const& value) { return emplace_front_stable(value); }
    /// Move-constructs a new element at the front using existing capacity.
    /// Precondition: !full().
    T& push_front_stable(T&& value) { return emplace_front_stable(cc::move(value)); }

    // insertion
public:
    /// Appends an element at the back, growing where the container can grow and asserting where it cannot.
    template <class... Args>
    T& emplace_back(Args&&... args)
    {
        _self()._ensure_capacity_for(1);
        return emplace_back_stable(cc::forward<Args>(args)...);
    }
    T& push_back(T const& value) { return emplace_back(value); }
    T& push_back(T&& value) { return emplace_back(cc::move(value)); }

    /// Prepends an element at the front, growing where the container can grow and asserting where it cannot.
    template <class... Args>
    T& emplace_front(Args&&... args)
    {
        _self()._ensure_capacity_for(1);
        return emplace_front_stable(cc::forward<Args>(args)...);
    }
    T& push_front(T const& value) { return emplace_front(value); }
    T& push_front(T&& value) { return emplace_front(cc::move(value)); }

    /// Appends at the back unless the ring is full, in which case nothing happens and the result is false.
    /// Never grows, so this is the bounded-queue spelling on a growable ring too.
    bool try_push_back(T const& value) { return _try_push_back(value); }
    bool try_push_back(T&& value) { return _try_push_back(cc::move(value)); }
    /// Prepends at the front unless the ring is full, in which case nothing happens and the result is false.
    bool try_push_front(T const& value) { return _try_push_front(value); }
    bool try_push_front(T&& value) { return _try_push_front(cc::move(value)); }

    /// Appends at the back, dropping the oldest element when the ring is full.
    /// Never grows, which is what makes it the "keep the newest capacity() elements" spelling.
    ///
    /// `value` must not alias the element being dropped.
    /// Precondition: capacity() > 0.
    T& push_back_overwriting(T const& value) { return _push_back_overwriting(value); }
    T& push_back_overwriting(T&& value) { return _push_back_overwriting(cc::move(value)); }

    /// Prepends at the front, dropping the newest element when the ring is full.
    ///
    /// `value` must not alias the element being dropped.
    /// Precondition: capacity() > 0.
    T& push_front_overwriting(T const& value) { return _push_front_overwriting(value); }
    T& push_front_overwriting(T&& value) { return _push_front_overwriting(cc::move(value)); }

    /// Appends every element of `range` at the back, reserving once.
    /// A contiguous range takes up to two block copies instead of a per-element loop.
    template <class Range>
    void push_back_range(Range const& range)
    {
        if constexpr (requires { cc::span<T const>(range); })
        {
            auto const source = cc::span<T const>(range);
            _self()._ensure_capacity_for(source.size());

            auto* const slots = _self()._slots();
            auto const cap = _self().capacity();
            auto const write = _self()._wrap(isize(_start) + size());
            auto const first = cc::min(source.size(), cap - write);

            auto* dst = slots + write;
            cc::impl::copy_create_objects_to(dst, source.data(), source.data() + first);
            _size = IndexT(size() + first); // per run, so a throwing copy leaves the live range correct

            dst = slots;
            cc::impl::copy_create_objects_to(dst, source.data() + first, source.data() + source.size());
            _size = IndexT(size() + (source.size() - first));
        }
        else
        {
            if constexpr (requires { range.size(); })
                _self()._ensure_capacity_for(isize(range.size()));

            for (auto const& e : range)
                push_back(e);
        }
    }

    /// Prepends `source` at the front, keeping its order — source[0] becomes the new front.
    /// Contiguous only, unlike push_back_range: prepending has to know the count and the order up front.
    void push_front_range(cc::span<T const> source)
    {
        _self()._ensure_capacity_for(source.size());
        if (source.empty())
            return;

        auto* const slots = _self()._slots();
        auto const cap = _self().capacity();
        auto const write = _self()._wrap(isize(_start) + cap - source.size());
        auto const first = cc::min(source.size(), cap - write);

        // The elements are constructed front to back, but only become live once all of them are:
        // _start moves last, so a throwing copy leaves the ring holding exactly its previous content.
        auto* dst = slots + write;
        cc::impl::copy_create_objects_to(dst, source.data(), source.data() + first);
        dst = slots;
        cc::impl::copy_create_objects_to(dst, source.data() + first, source.data() + source.size());

        _start = IndexT(write);
        _size = IndexT(size() + source.size());
    }

    // removal
public:
    /// Removes and returns the oldest element by move.
    /// Precondition: !empty().
    [[nodiscard("use remove_front() if you don't need the return value")]] T pop_front()
    {
        CC_ASSERT(!empty(), "pop_front() on empty ringbuffer");
        auto value = cc::move(*_slot(0));
        remove_front();
        return value;
    }
    /// Removes the oldest element.
    /// Precondition: !empty().
    void remove_front()
    {
        CC_ASSERT(!empty(), "remove_front() on empty ringbuffer");
        _slot(0)->~T();
        _start = IndexT(_self()._wrap(isize(_start) + 1));
        _size = IndexT(size() - 1);
    }

    /// Removes and returns the newest element by move.
    /// Precondition: !empty().
    [[nodiscard("use remove_back() if you don't need the return value")]] T pop_back()
    {
        CC_ASSERT(!empty(), "pop_back() on empty ringbuffer");
        auto value = cc::move(*_slot(size() - 1));
        remove_back();
        return value;
    }
    /// Removes the newest element.
    /// Precondition: !empty().
    void remove_back()
    {
        CC_ASSERT(!empty(), "remove_back() on empty ringbuffer");
        _slot(size() - 1)->~T();
        _size = IndexT(size() - 1);
    }

    /// Removes and returns the oldest element, or cc::nullopt on an empty ring.
    [[nodiscard]] cc::optional<T> try_pop_front()
    {
        if (empty())
            return {};
        return pop_front();
    }
    /// Removes and returns the newest element, or cc::nullopt on an empty ring.
    [[nodiscard]] cc::optional<T> try_pop_back()
    {
        if (empty())
            return {};
        return pop_back();
    }

    /// Removes the `count` oldest elements.
    /// Precondition: 0 <= count <= size().
    void remove_front_n(isize count)
    {
        CC_ASSERT(0 <= count && count <= size(), "remove_front_n count out of range");
        for (isize i = 0; i < count; ++i)
            _slot(i)->~T();
        _start = IndexT(_self()._wrap(isize(_start) + count));
        _size = IndexT(size() - count);
    }
    /// Removes the `count` newest elements.
    /// Precondition: 0 <= count <= size().
    void remove_back_n(isize count)
    {
        CC_ASSERT(0 <= count && count <= size(), "remove_back_n count out of range");
        for (isize i = 0; i < count; ++i)
            _slot(size() - 1 - i)->~T();
        _size = IndexT(size() - count);
    }

    /// Destroys every element; size becomes 0 and the capacity is unchanged.
    void clear()
    {
        auto const [a, b] = segments();
        cc::impl::destroy_objects_in_reverse(b.begin(), b.end());
        cc::impl::destroy_objects_in_reverse(a.begin(), a.end());
        _start = 0;
        _size = 0;
    }

    // other operations
public:
    /// Copy-assigns the elements front to back over the live elements of `dest`.
    /// Precondition: dest.size() >= size().
    void copy_to(cc::span<T> dest) const
    {
        CC_ASSERT(dest.size() >= size(), "copy_to destination is too small");
        auto const [a, b] = segments();
        auto* cursor = dest.data();
        cc::impl::copy_assign_objects_to(cursor, a.begin(), a.end());
        cc::impl::copy_assign_objects_to(cursor, b.begin(), b.end());
    }

    /// Assigns `value` to every live element; size unchanged.
    void fill(T const& value)
    {
        for (auto& e : *this)
            e = value;
    }

    // implementation
public:
    /// Slot of the front element, always < capacity(), and 0 for an empty ring.
    IndexT _start = 0;
    /// Live element count.
    IndexT _size = 0;

private:
    [[nodiscard]] constexpr ContainerT& _self() { return static_cast<ContainerT&>(*this); }
    [[nodiscard]] constexpr ContainerT const& _self() const { return static_cast<ContainerT const&>(*this); }

    /// Address of the i-th element from the front.
    /// _start < capacity() and i < capacity() keep the sum inside _wrap's [0, 2 * capacity()) domain.
    [[nodiscard]] constexpr T* _slot(isize i) { return _self()._slots() + _self()._wrap(isize(_start) + i); }
    [[nodiscard]] constexpr T const* _slot(isize i) const
    {
        return _self()._slots() + _self()._wrap(isize(_start) + i);
    }

    /// Length of the run starting at _start, which is the whole content unless it wraps.
    [[nodiscard]] constexpr isize _first_run_length() const
    {
        return cc::min(size(), _self().capacity() - isize(_start));
    }

    /// Whether `slot` held a live element in a layout of `count` elements starting at `start`.
    /// Only linearize needs this: it walks all `cap` slots, live or not.
    [[nodiscard]] static constexpr bool _slot_was_live(isize slot, isize start, isize count, isize cap)
    {
        auto rel = slot - start;
        if (rel < 0)
            rel += cap;
        return rel < count;
    }

    template <class U>
    bool _try_push_back(U&& value)
    {
        if (full())
            return false;
        emplace_back_stable(cc::forward<U>(value));
        return true;
    }
    template <class U>
    bool _try_push_front(U&& value)
    {
        if (full())
            return false;
        emplace_front_stable(cc::forward<U>(value));
        return true;
    }

    template <class U>
    T& _push_back_overwriting(U&& value)
    {
        CC_ASSERT(_self().capacity() > 0, "push_back_overwriting needs capacity, reserve it first");
        if (full())
            remove_front();
        return emplace_back_stable(cc::forward<U>(value));
    }
    template <class U>
    T& _push_front_overwriting(U&& value)
    {
        CC_ASSERT(_self().capacity() > 0, "push_front_overwriting needs capacity, reserve it first");
        if (full())
            remove_back();
        return emplace_front_stable(cc::forward<U>(value));
    }
};
