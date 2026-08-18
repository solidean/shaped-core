#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh> // cc::make_hash_range
#include <clean-core/common/impl/small_size_type.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>
#include <clean-core/memory/impl/object_lifetime_util.hh> // the shared gap-resize primitive behind the insertion family

#include <initializer_list>
#include <type_traits>

/// Fixed-capacity vector: up to `N` elements of `T` stored inline, with a runtime-variable size and **no dynamic allocation ever**.
/// The size grows and shrinks like a vector, but the capacity is a hard compile-time cap — pushing past `N` asserts instead of spilling to the heap.
///
/// Reach for it where `N` is a genuine invariant, such as a hardware or protocol limit, rather than a size hint.
/// The type then *encodes* the cap, so an overflow is a bug that trips instead of a silent allocation.
/// Where an occasional overflow must still work, use `cc::small_vector` instead.
///
/// The public surface mirrors `cc::vector` wherever that is meaningful for a fixed capacity, so it is a drop-in where the capacity fits.
/// Members that exist only to manage a growable allocation are intentionally absent — `reserve*`, `shrink_to_fit`, `create_with_capacity`,
/// `create_from_allocation` and `extract_allocation` have nothing left to do when the capacity is always `N`.
///
/// Value semantics (deep copy); a moved-from fixed_vector is left empty.
/// Elements are constructed and destroyed in place, so `T` need not be default-constructible.
/// `N == 0` is a valid, permanently empty vector.
///
/// **Not subobject-safe:** copy/move assignment destroys the current elements before reading the source.
/// Assigning from an element of the *same* vector (`v = v[0].other`) is therefore undefined behavior — the self-assignment guard only catches whole-object aliasing.
/// Same constraint as `cc::optional`, and unproblematic for a non-allocating container.
///
/// See [containers](../../../docs/containers.md) for the contracts every container shares, including what removal does to references.
///
/// Usage:
///   cc::fixed_vector<int, 4> v; // holds at most 4 ints, never allocates
///   v.push_back(1);
///   v.emplace_back(2);
///   for (int x : v) { ... }
template <class T, cc::isize N>
struct cc::fixed_vector
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>, "fixed_vector needs non-const object elements");
    static_assert(N >= 0, "fixed_vector capacity N must be non-negative");

    // factories (mirroring cc::vector)
public:
    /// A deep copy of `source`.
    /// Precondition: source.size() <= N.
    [[nodiscard]] static fixed_vector create_copy_of(cc::span<T const> source)
    {
        CC_ASSERT(source.size() <= N, "create_copy_of exceeds fixed_vector capacity");
        fixed_vector v;
        for (auto const& e : source)
            v.push_back(e);
        return v;
    }

    /// `size` default-constructed (value-initialized) elements.
    /// Precondition: 0 <= size <= N.
    [[nodiscard]] static fixed_vector create_defaulted(isize size)
    {
        fixed_vector v;
        v.resize_to_defaulted(size);
        return v;
    }

    /// `size` elements, each a copy of `value`.
    /// Precondition: 0 <= size <= N.
    [[nodiscard]] static fixed_vector create_filled(isize size, T const& value)
    {
        fixed_vector v;
        v.resize_to_filled(size, value);
        return v;
    }

    /// `size` uninitialized elements (trivial types only) — the caller must fill them before reading.
    [[nodiscard]] static fixed_vector create_uninitialized(isize size)
    {
        fixed_vector v;
        v.resize_to_uninitialized(size);
        return v;
    }

    // ctors / dtor / assignment
public:
    // User-provided rather than `= default`, so a const fixed_vector is const-default-constructible.
    // The same holds for any aggregate that stores one without a default member initializer.
    // Leaves the storage uninitialized; `_size` starts at 0 through its own default member initializer.
    fixed_vector() {}

    fixed_vector(std::initializer_list<T> init)
    {
        CC_ASSERT(isize(init.size()) <= N, "fixed_vector initializer exceeds capacity");
        for (auto const& e : init)
            push_back(e);
    }

    fixed_vector(fixed_vector const& rhs)
    {
        for (isize i = 0; i < rhs.size(); ++i)
            push_back(rhs[i]);
    }

    fixed_vector(fixed_vector&& rhs) noexcept
    {
        for (isize i = 0; i < rhs.size(); ++i)
            emplace_back(cc::move(rhs[i]));
        rhs.clear();
    }

    fixed_vector& operator=(fixed_vector const& rhs)
    {
        if (this != &rhs)
        {
            clear();
            for (isize i = 0; i < rhs.size(); ++i)
                push_back(rhs[i]);
        }
        return *this;
    }

    fixed_vector& operator=(fixed_vector&& rhs) noexcept
    {
        if (this != &rhs)
        {
            clear();
            for (isize i = 0; i < rhs.size(); ++i)
                emplace_back(cc::move(rhs[i]));
            rhs.clear();
        }
        return *this;
    }

    ~fixed_vector() { clear(); }

    // element access
public:
    [[nodiscard]] T& operator[](isize i)
    {
        CC_ASSERT(i >= 0 && i < size(), "fixed_vector index out of bounds");
        return data()[i];
    }
    [[nodiscard]] T const& operator[](isize i) const
    {
        CC_ASSERT(i >= 0 && i < size(), "fixed_vector index out of bounds");
        return data()[i];
    }

    [[nodiscard]] T& front()
    {
        CC_ASSERT(!empty(), "front() on empty fixed_vector");
        return data()[0];
    }
    [[nodiscard]] T const& front() const
    {
        CC_ASSERT(!empty(), "front() on empty fixed_vector");
        return data()[0];
    }
    [[nodiscard]] T& back()
    {
        CC_ASSERT(!empty(), "back() on empty fixed_vector");
        return data()[size() - 1];
    }
    [[nodiscard]] T const& back() const
    {
        CC_ASSERT(!empty(), "back() on empty fixed_vector");
        return data()[size() - 1];
    }

    [[nodiscard]] T* data() { return reinterpret_cast<T*>(_storage); }
    [[nodiscard]] T const* data() const { return reinterpret_cast<T const*>(_storage); }

    // iterators
public:
    [[nodiscard]] T* begin() { return data(); }
    [[nodiscard]] T* end() { return data() + size(); }
    [[nodiscard]] T const* begin() const { return data(); }
    [[nodiscard]] T const* end() const { return data() + size(); }

    // queries
public:
    /// Element count as isize — the storage keeps it in a smaller unsigned type, converted here.
    [[nodiscard]] isize size() const { return isize(_size); }
    [[nodiscard]] bool empty() const { return _size == 0; }
    [[nodiscard]] bool full() const { return size() == N; }
    [[nodiscard]] isize size_bytes() const { return size() * isize(sizeof(T)); }

    /// The compile-time capacity — the hard cap on element count.
    [[nodiscard]] static constexpr isize capacity() { return N; }
    [[nodiscard]] isize capacity_back() const { return N - size(); }
    [[nodiscard]] bool has_capacity_back_for(isize count) const { return capacity_back() >= count; }

    // appending — no `_stable` variants: a fixed_vector never reallocates, so every append is already pointer-stable.
    // The stable/unstable distinction cc::vector draws is meaningless here.
public:
    void push_back(T const& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(cc::move(value)); }

    template <class... Args>
    T& emplace_back(Args&&... args)
    {
        CC_ASSERT(size() < N, "fixed_vector capacity exceeded");
        T* const slot = data() + size();
        new (cc::placement_new, slot) T(cc::forward<Args>(args)...);
        ++_size;
        return *slot;
    }

    /// Appends every element of `range` to the back.
    /// Precondition: size() + <range size> <= N; a sized range checks that up front rather than one element short of the end.
    template <class Range>
    void push_back_range(Range const& range)
    {
        if constexpr (requires { isize(range.size()); })
            CC_ASSERT(has_capacity_back_for(isize(range.size())), "push_back_range exceeds fixed_vector capacity");

        for (auto&& e : range)
            emplace_back(cc::forward<decltype(e)>(e));
    }

    // insertion at a position
public:
    /// Constructs an element at `idx` from `args...`, shifting everything at or after `idx` up by one, and returns a reference to it.
    /// The element is built into a temporary first, so `v.emplace_at(i, v[0])` is safe and a throwing constructor leaves the vector unchanged.
    /// `idx == size()` is the append position and is legal.
    /// Precondition: 0 <= idx <= size() and size() < N.
    /// O(size() - idx).
    template <class... Args>
    T& emplace_at(isize idx, Args&&... args)
    {
        CC_ASSERT(idx >= 0 && idx <= size(), "emplace_at index out of bounds");
        CC_ASSERT(size() < N, "fixed_vector capacity exceeded");

        // Built before anything moves, so args may reference this vector's own elements.
        auto value = T(cc::forward<Args>(args)...);

        T* const d = data();
        isize const old_size = size();
        cc::impl::resize_object_window(d + idx, 0, 1, old_size - idx);

        // While the hole is open the tail is parked above it, alive but outside the size, so a throw truncates instead of leaking.
        auto guard = gap_guard{.start = d + idx + 1, .end = d + old_size + 1};
        _size = size_type(idx);
        T* const p = new (cc::placement_new, d + idx) T(cc::move(value));
        guard.dismiss();
        _size = size_type(old_size + 1);

        return *p;
    }

    /// Inserts a copy of `value` at `idx`.
    /// See emplace_at for guarantees and complexity.
    T& insert_at(isize idx, T const& value) { return emplace_at(idx, value); }

    /// Inserts `value` at `idx` by move.
    /// See emplace_at for guarantees and complexity.
    T& insert_at(isize idx, T&& value) { return emplace_at(idx, cc::move(value)); }

    /// Inserts every element of `range` at `idx`, and returns the span of the newly inserted elements.
    /// `range` must be sized, and must not alias this vector's own elements.
    /// Precondition: 0 <= idx <= size() and size() + <range size> <= N.
    template <class Range>
    cc::span<T> insert_range_at(isize idx, Range const& range)
    {
        CC_ASSERT(idx >= 0 && idx <= size(), "insert_range_at index out of bounds");
        return replace_range(idx, 0, range);
    }

    /// Replaces the `count` elements at `start` with every element of `range`, and returns the span of the elements now there.
    /// The tail behind the replaced run moves exactly once, whether `range` is shorter, equal or longer than what it replaces.
    /// `range` must be SIZED, and must not alias this vector's own elements.
    /// The replaced run frees room, so a full fixed_vector still takes an equal-sized replacement.
    /// Precondition: 0 <= start && 0 <= count && start + count <= size() and size() - count + <range size> <= N.
    template <class Range>
    cc::span<T> replace_range(isize start, isize count, Range const& range)
    {
        static_assert(
            requires(Range const& r) { isize(r.size()); },
            "replace_range / insert_range_at need a SIZED range (one exposing .size()): the gap has to be "
            "opened before any element can be read. Materialize the range into a cc::vector first.");
        CC_ASSERT(start >= 0 && count >= 0 && start + count <= size(), "replace_range out of bounds");

        isize const new_count = isize(range.size());
        isize const old_size = size();
        CC_ASSERT(old_size - count + new_count <= N, "replace_range exceeds fixed_vector capacity");

        T* const d = data();

        if constexpr (requires(Range const& r) {
                          r.data();
                          requires std::is_same_v<std::remove_cv_t<std::remove_pointer_t<decltype(r.data())>>, T>;
                      })
        {
            auto const* const p_src = range.data();
            CC_ASSERT(new_count == 0 || p_src + new_count <= d || p_src >= d + old_size,
                      "replace_range source must not alias the vector's own elements — copy it first");
        }

        isize const tail_count = old_size - start - count;
        cc::impl::resize_object_window(d + start, count, new_count, tail_count);

        auto guard = gap_guard{.start = d + start + new_count, .end = d + start + new_count + tail_count};
        _size = size_type(start);
        for (auto&& e : range)
        {
            new (cc::placement_new, d + size()) T(cc::forward<decltype(e)>(e));
            _size = size_type(size() + 1);
        }
        CC_ASSERT(size() == start + new_count, "range.size() disagreed with the elements the range yielded");
        guard.dismiss();
        _size = size_type(start + new_count + tail_count);

        return cc::span<T>(d + start, new_count);
    }

    // single element removal
public:
    /// Removes and returns the last element.
    /// Precondition: !empty().
    [[nodiscard]] T pop_back()
    {
        CC_ASSERT(!empty(), "pop_back() on empty fixed_vector");
        T value = cc::move(back());
        remove_back();
        return value;
    }

    /// Removes the last element without returning it.
    /// Precondition: !empty().
    void remove_back()
    {
        CC_ASSERT(!empty(), "remove_back() on empty fixed_vector");
        isize const last = size() - 1;
        data()[last].~T();
        _size = size_type(last);
    }

    /// Removes and returns the element at `idx`, preserving order.
    /// Precondition: 0 <= idx < size().
    [[nodiscard]] T pop_at(isize idx)
    {
        T value = cc::move((*this)[idx]);
        remove_at(idx);
        return value;
    }
    /// Removes the element at `idx`, preserving order (O(n) compaction).
    /// Precondition: 0 <= idx < size().
    void remove_at(isize idx)
    {
        CC_ASSERT(idx >= 0 && idx < size(), "remove_at index out of bounds");
        T* const d = data();
        for (isize i = idx; i + 1 < size(); ++i)
            d[i] = cc::move(d[i + 1]);
        remove_back();
    }
    /// Removes and returns the element at `idx` by swapping in the last element (O(1), unordered).
    [[nodiscard]] T pop_at_unordered(isize idx)
    {
        T value = cc::move((*this)[idx]);
        remove_at_unordered(idx);
        return value;
    }
    /// Removes the element at `idx` by swapping in the last element (O(1), does not preserve order).
    void remove_at_unordered(isize idx)
    {
        CC_ASSERT(idx >= 0 && idx < size(), "remove_at_unordered index out of bounds");
        if (idx != size() - 1)
            data()[idx] = cc::move(data()[size() - 1]);
        remove_back();
    }

    // range removal
public:
    /// Removes `count` elements starting at `start`, preserving order.
    /// Precondition: start + count <= size().
    void remove_at_range(isize start, isize count)
    {
        CC_ASSERT(start >= 0 && count >= 0 && start + count <= size(), "remove_at_range out of bounds");
        T* const d = data();
        isize const new_size = size() - count;
        for (isize i = start; i < new_size; ++i)
            d[i] = cc::move(d[i + count]);
        _shrink_to(new_size);
    }
    /// Removes `count` elements starting at `start` by moving trailing elements into the gap (unordered).
    void remove_at_range_unordered(isize start, isize count)
    {
        CC_ASSERT(start >= 0 && count >= 0 && start + count <= size(), "remove_at_range_unordered out of bounds");
        T* const d = data();
        isize const avail = size() - (start + count);  // untouched elements after the removed range
        isize const k = avail < count ? avail : count; // how many tail elements move into the gap
        for (isize i = 0; i < k; ++i)
            d[start + i] = cc::move(d[size() - k + i]);
        _shrink_to(size() - count);
    }
    /// Removes the range [start, end), preserving order.
    /// Precondition: start <= end <= size().
    void remove_from_to(isize start, isize end)
    {
        CC_ASSERT(start >= 0 && start <= end && end <= size(), "remove_from_to out of bounds");
        remove_at_range(start, end - start);
    }
    /// Removes the range [start, end) by moving trailing elements into the gap (unordered).
    void remove_from_to_unordered(isize start, isize end)
    {
        CC_ASSERT(start >= 0 && start <= end && end <= size(), "remove_from_to_unordered out of bounds");
        remove_at_range_unordered(start, end - start);
    }

    // predicate-based removal
public:
    /// Removes every element for which `pred` is true (preserving order); returns the number removed.
    template <class Pred>
    isize remove_all_where(Pred&& pred)
    {
        T* const d = data();
        isize w = 0;
        for (isize r = 0; r < size(); ++r)
            if (!pred(d[r]))
            {
                if (w != r)
                    d[w] = cc::move(d[r]);
                ++w;
            }
        isize const removed = size() - w;
        _shrink_to(w);
        return removed;
    }
    /// Removes the first element for which `pred` is true (preserving order); returns whether one was removed.
    template <class Pred>
    bool remove_first_where(Pred&& pred)
    {
        for (isize i = 0; i < size(); ++i)
            if (pred(data()[i]))
            {
                remove_at(i);
                return true;
            }
        return false;
    }
    /// Removes the last element for which `pred` is true (preserving order); returns whether one was removed.
    template <class Pred>
    bool remove_last_where(Pred&& pred)
    {
        for (isize i = size() - 1; i >= 0; --i)
            if (pred(data()[i]))
            {
                remove_at(i);
                return true;
            }
        return false;
    }

    /// Removes every element equal to `value` (preserving order); returns the number removed.
    isize remove_all_value(T const& value)
    {
        return remove_all_where([&](T const& e) { return e == value; });
    }
    /// Removes the first element equal to `value` (preserving order); returns whether one was removed.
    bool remove_first_value(T const& value)
    {
        return remove_first_where([&](T const& e) { return e == value; });
    }
    /// Removes the last element equal to `value` (preserving order); returns whether one was removed.
    bool remove_last_value(T const& value)
    {
        return remove_last_where([&](T const& e) { return e == value; });
    }

    /// Keeps only the elements for which `pred` is true (preserving order); returns the number removed.
    template <class Pred>
    isize retain_all_where(Pred&& pred)
    {
        return remove_all_where([&](T const& e) { return !pred(e); });
    }

    // resizing
public:
    /// Shrinks to `new_size` by destroying trailing elements.
    /// Precondition: 0 <= new_size <= size().
    void resize_down_to(isize new_size)
    {
        CC_ASSERT(new_size >= 0 && new_size <= size(), "resize_down_to must not grow");
        _shrink_to(new_size);
    }
    /// Resizes to `new_size`; new elements are `T(args...)`.
    /// Precondition: 0 <= new_size <= N.
    template <class... Args>
    void resize_to_constructed(isize new_size, Args const&... args)
    {
        CC_ASSERT(new_size >= 0 && new_size <= N, "resize_to_constructed exceeds fixed_vector capacity");
        if (new_size < size())
            _shrink_to(new_size);
        else
            while (size() < new_size)
                emplace_back(args...);
    }
    /// Resizes to `new_size`, default-constructing any new elements.
    /// Precondition: 0 <= new_size <= N.
    void resize_to_defaulted(isize new_size)
    {
        CC_ASSERT(new_size >= 0 && new_size <= N, "resize_to_defaulted exceeds fixed_vector capacity");
        if (new_size < size())
            _shrink_to(new_size);
        else
            while (size() < new_size)
                emplace_back();
    }
    /// Resizes to `new_size`, filling any new elements with `value`.
    /// Precondition: 0 <= new_size <= N.
    void resize_to_filled(isize new_size, T const& value)
    {
        CC_ASSERT(new_size >= 0 && new_size <= N, "resize_to_filled exceeds fixed_vector capacity");
        if (new_size < size())
            _shrink_to(new_size);
        else
            while (size() < new_size)
                push_back(value);
    }
    /// Resizes to `new_size`, leaving any new elements uninitialized (trivial types only).
    /// Existing elements are kept.
    /// Precondition: 0 <= new_size <= N.
    void resize_to_uninitialized(isize new_size)
    {
        CC_ASSERT(new_size >= 0 && new_size <= N, "resize_to_uninitialized exceeds fixed_vector capacity");
        if (new_size < size())
            _shrink_to(new_size);
        else
            _size = size_type(new_size); // new elements uninitialized — valid for trivially-constructible T only
    }

    template <class... Args>
    void clear_resize_to_constructed(isize new_size, Args const&... args)
    {
        clear();
        resize_to_constructed(new_size, args...);
    }
    void clear_resize_to_defaulted(isize new_size)
    {
        clear();
        resize_to_defaulted(new_size);
    }
    void clear_resize_to_filled(isize new_size, T const& value)
    {
        clear();
        resize_to_filled(new_size, value);
    }
    void clear_resize_to_uninitialized(isize new_size)
    {
        clear();
        resize_to_uninitialized(new_size);
    }

    // other mutations
public:
    /// Destroys every element (size becomes 0; capacity is unchanged).
    void clear()
    {
        T* const d = data();
        for (isize i = 0; i < size(); ++i)
            d[i].~T();
        _size = 0;
    }

    /// Assigns `value` to every current element (size unchanged).
    void fill(T const& value)
    {
        T* const d = data();
        for (isize i = 0; i < size(); ++i)
            d[i] = value;
    }

    // hashing
public:
    /// Structural, order-dependent hash over the live elements.
    [[nodiscard]] friend u64 hash(fixed_vector const& v) { return cc::make_hash_range(v); }

    // implementation
private:
    /// Destroys the tail parked above an open insertion gap, unless it has been dismissed.
    /// While the gap is open that tail is alive but sits outside the vector's size, so without this a throwing element constructor would skip its destructors.
    struct gap_guard
    {
        T* start = nullptr;
        T* end = nullptr;

        void dismiss() { start = end = nullptr; }
        ~gap_guard() { cc::impl::destroy_objects_in_reverse(start, end); }
    };

    /// Destroys elements [new_size, size()) and sets the size.
    /// Precondition: 0 <= new_size <= size().
    void _shrink_to(isize new_size)
    {
        T* const d = data();
        for (isize i = new_size; i < size(); ++i)
            d[i].~T();
        _size = size_type(new_size);
    }

    // Count field of the smallest unsigned type that holds N and fills the tail padding next to `alignas(T)`
    // storage (u8 for fixed_vector<u8,10>, u16 for fixed_vector<u8,300>, u64 for fixed_vector<u64,2>, ...).
    // Purely a storage detail — every read goes through size(), which converts to the signed isize API.
    using size_type = cc::impl::small_size_t<u64(N), alignof(T)>;

    // Uninitialized aligned storage for N elements; only [0, _size) are alive.
    // The reinterpret_cast in data() is well-defined for objects placement-new'd into this buffer.
    // Sized to at least 1 byte so N == 0 does not form a zero-length array, which would be UB.
    alignas(T) unsigned char _storage[N == 0 ? 1 : sizeof(T) * N];
    size_type _size = 0;
};
