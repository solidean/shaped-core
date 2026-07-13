#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh> // cc::make_hash_range
#include <clean-core/common/utility.hh>
#include <clean-core/container/span.hh>
#include <clean-core/fwd.hh>

#include <initializer_list>
#include <type_traits>

/// Fixed-capacity vector: up to `N` elements of `T` stored inline, with runtime-variable size and **no
/// dynamic allocation ever**. The size grows and shrinks like a vector, but the capacity is a hard
/// compile-time cap — pushing past `N` asserts (it does not spill to the heap like `small_vector`).
///
/// Use it where `N` is a genuine invariant — a hardware / protocol limit — rather than a size hint: the
/// type then *encodes* the cap (an overflow is a bug that trips, not a silent allocation). For "usually a
/// handful but an occasional overflow must still work", use `cc::small_vector` instead.
///
/// The public surface mirrors `cc::vector` where it is meaningful for a fixed capacity (create_*
/// factories, push/emplace [_stable], pop/remove, remove_at[_unordered] / _range / _where, resize_* /
/// clear_resize_* family, fill), so it is a drop-in where the capacity fits. Members that only exist to
/// manage a growable allocation — `reserve*`, `shrink_to_fit`, `create_with_capacity` /
/// `create_from_allocation` / `extract_allocation` — are meaningless here (capacity is always `N`) and are
/// intentionally absent.
///
/// Value semantics (deep copy); a moved-from fixed_vector is left empty. Elements are constructed /
/// destroyed in place, so `T` need not be default-constructible.
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
    static_assert(N >= 1, "fixed_vector capacity N must be >= 1");

    // factories (mirroring cc::vector)
public:
    /// A deep copy of `source`. Precondition: source.size() <= N.
    [[nodiscard]] static fixed_vector create_copy_of(cc::span<T const> source)
    {
        CC_ASSERT(source.size() <= N, "create_copy_of exceeds fixed_vector capacity");
        fixed_vector v;
        for (auto const& e : source)
            v.push_back(e);
        return v;
    }

    /// `size` default-constructed (value-initialized) elements. Precondition: 0 <= size <= N.
    [[nodiscard]] static fixed_vector create_defaulted(isize size)
    {
        fixed_vector v;
        v.resize_to_defaulted(size);
        return v;
    }

    /// `size` elements, each a copy of `value`. Precondition: 0 <= size <= N.
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
    // User-provided (not =default) so a const fixed_vector — and any aggregate holding one without a
    // default member initializer — is const-default-constructible, matching cc::small_vector. Leaves the
    // storage uninitialized; `_size` starts at 0 via its default member initializer.
    fixed_vector() {}

    fixed_vector(std::initializer_list<T> init)
    {
        CC_ASSERT(isize(init.size()) <= N, "fixed_vector initializer exceeds capacity");
        for (auto const& e : init)
            push_back(e);
    }

    fixed_vector(fixed_vector const& rhs)
    {
        for (isize i = 0; i < rhs._size; ++i)
            push_back(rhs[i]);
    }

    fixed_vector(fixed_vector&& rhs) noexcept
    {
        for (isize i = 0; i < rhs._size; ++i)
            emplace_back(cc::move(rhs[i]));
        rhs.clear();
    }

    fixed_vector& operator=(fixed_vector const& rhs)
    {
        if (this != &rhs)
        {
            clear();
            for (isize i = 0; i < rhs._size; ++i)
                push_back(rhs[i]);
        }
        return *this;
    }

    fixed_vector& operator=(fixed_vector&& rhs) noexcept
    {
        if (this != &rhs)
        {
            clear();
            for (isize i = 0; i < rhs._size; ++i)
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
        CC_ASSERT(i >= 0 && i < _size, "fixed_vector index out of bounds");
        return data()[i];
    }
    [[nodiscard]] T const& operator[](isize i) const
    {
        CC_ASSERT(i >= 0 && i < _size, "fixed_vector index out of bounds");
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
        return data()[_size - 1];
    }
    [[nodiscard]] T const& back() const
    {
        CC_ASSERT(!empty(), "back() on empty fixed_vector");
        return data()[_size - 1];
    }

    [[nodiscard]] T* data() { return reinterpret_cast<T*>(_storage); }
    [[nodiscard]] T const* data() const { return reinterpret_cast<T const*>(_storage); }

    // iterators
public:
    [[nodiscard]] T* begin() { return data(); }
    [[nodiscard]] T* end() { return data() + _size; }
    [[nodiscard]] T const* begin() const { return data(); }
    [[nodiscard]] T const* end() const { return data() + _size; }

    // queries
public:
    [[nodiscard]] isize size() const { return _size; }
    [[nodiscard]] bool empty() const { return _size == 0; }
    [[nodiscard]] bool full() const { return _size == N; }
    [[nodiscard]] isize size_bytes() const { return _size * isize(sizeof(T)); }

    /// The compile-time capacity — the hard cap on element count.
    [[nodiscard]] static constexpr isize capacity() { return N; }
    [[nodiscard]] isize capacity_back() const { return N - _size; }
    [[nodiscard]] bool has_capacity_back_for(isize count) const { return capacity_back() >= count; }

    // appending — no `_stable` variants: a fixed_vector never reallocates, so every append is already
    // pointer-stable and the stable/unstable distinction cc::vector draws is meaningless here.
public:
    void push_back(T const& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(cc::move(value)); }

    template <class... Args>
    T& emplace_back(Args&&... args)
    {
        CC_ASSERT(_size < N, "fixed_vector capacity exceeded");
        T* const slot = data() + _size;
        new (cc::placement_new, slot) T(cc::forward<Args>(args)...);
        ++_size;
        return *slot;
    }

    // single element removal
public:
    /// Removes and returns the last element. Precondition: !empty().
    [[nodiscard]] T pop_back()
    {
        CC_ASSERT(!empty(), "pop_back() on empty fixed_vector");
        T value = cc::move(back());
        remove_back();
        return value;
    }

    /// Removes the last element without returning it. Precondition: !empty().
    void remove_back()
    {
        CC_ASSERT(!empty(), "remove_back() on empty fixed_vector");
        --_size;
        data()[_size].~T();
    }

    /// Removes and returns the element at `idx`, preserving order. Precondition: 0 <= idx < size().
    [[nodiscard]] T pop_at(isize idx)
    {
        T value = cc::move((*this)[idx]);
        remove_at(idx);
        return value;
    }
    /// Removes the element at `idx`, preserving order (O(n) compaction). Precondition: 0 <= idx < size().
    void remove_at(isize idx)
    {
        CC_ASSERT(idx >= 0 && idx < _size, "remove_at index out of bounds");
        T* const d = data();
        for (isize i = idx; i + 1 < _size; ++i)
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
        CC_ASSERT(idx >= 0 && idx < _size, "remove_at_unordered index out of bounds");
        if (idx != _size - 1)
            data()[idx] = cc::move(data()[_size - 1]);
        remove_back();
    }

    // range removal
public:
    /// Removes `count` elements starting at `start`, preserving order. Precondition: start + count <= size().
    void remove_at_range(isize start, isize count)
    {
        CC_ASSERT(start >= 0 && count >= 0 && start + count <= _size, "remove_at_range out of bounds");
        T* const d = data();
        isize const new_size = _size - count;
        for (isize i = start; i < new_size; ++i)
            d[i] = cc::move(d[i + count]);
        _shrink_to(new_size);
    }
    /// Removes `count` elements starting at `start` by moving trailing elements into the gap (unordered).
    void remove_at_range_unordered(isize start, isize count)
    {
        CC_ASSERT(start >= 0 && count >= 0 && start + count <= _size, "remove_at_range_unordered out of bounds");
        T* const d = data();
        isize const avail = _size - (start + count);   // untouched elements after the removed range
        isize const k = avail < count ? avail : count; // how many tail elements move into the gap
        for (isize i = 0; i < k; ++i)
            d[start + i] = cc::move(d[_size - k + i]);
        _shrink_to(_size - count);
    }
    /// Removes the range [start, end), preserving order. Precondition: start <= end <= size().
    void remove_from_to(isize start, isize end)
    {
        CC_ASSERT(start >= 0 && start <= end && end <= _size, "remove_from_to out of bounds");
        remove_at_range(start, end - start);
    }
    /// Removes the range [start, end) by moving trailing elements into the gap (unordered).
    void remove_from_to_unordered(isize start, isize end)
    {
        CC_ASSERT(start >= 0 && start <= end && end <= _size, "remove_from_to_unordered out of bounds");
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
        for (isize r = 0; r < _size; ++r)
            if (!pred(d[r]))
            {
                if (w != r)
                    d[w] = cc::move(d[r]);
                ++w;
            }
        isize const removed = _size - w;
        _shrink_to(w);
        return removed;
    }
    /// Removes the first element for which `pred` is true (preserving order); returns whether one was removed.
    template <class Pred>
    bool remove_first_where(Pred&& pred)
    {
        for (isize i = 0; i < _size; ++i)
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
        for (isize i = _size - 1; i >= 0; --i)
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
    /// Shrinks to `new_size` by destroying trailing elements. Precondition: 0 <= new_size <= size().
    void resize_down_to(isize new_size)
    {
        CC_ASSERT(new_size >= 0 && new_size <= _size, "resize_down_to must not grow");
        _shrink_to(new_size);
    }
    /// Resizes to `new_size`; new elements are `T(args...)`. Precondition: 0 <= new_size <= N.
    template <class... Args>
    void resize_to_constructed(isize new_size, Args const&... args)
    {
        CC_ASSERT(new_size >= 0 && new_size <= N, "resize_to_constructed exceeds fixed_vector capacity");
        if (new_size < _size)
            _shrink_to(new_size);
        else
            while (_size < new_size)
                emplace_back(args...);
    }
    /// Resizes to `new_size`, default-constructing any new elements. Precondition: 0 <= new_size <= N.
    void resize_to_defaulted(isize new_size)
    {
        CC_ASSERT(new_size >= 0 && new_size <= N, "resize_to_defaulted exceeds fixed_vector capacity");
        if (new_size < _size)
            _shrink_to(new_size);
        else
            while (_size < new_size)
                emplace_back();
    }
    /// Resizes to `new_size`, filling any new elements with `value`. Precondition: 0 <= new_size <= N.
    void resize_to_filled(isize new_size, T const& value)
    {
        CC_ASSERT(new_size >= 0 && new_size <= N, "resize_to_filled exceeds fixed_vector capacity");
        if (new_size < _size)
            _shrink_to(new_size);
        else
            while (_size < new_size)
                push_back(value);
    }
    /// Resizes to `new_size`, leaving any new elements uninitialized (trivial types only); keeps existing.
    void resize_to_uninitialized(isize new_size)
    {
        CC_ASSERT(new_size >= 0 && new_size <= N, "resize_to_uninitialized exceeds fixed_vector capacity");
        if (new_size < _size)
            _shrink_to(new_size);
        else
            _size = new_size; // new elements uninitialized — valid for trivially-constructible T only
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
        for (isize i = 0; i < _size; ++i)
            d[i].~T();
        _size = 0;
    }

    /// Assigns `value` to every current element (size unchanged).
    void fill(T const& value)
    {
        T* const d = data();
        for (isize i = 0; i < _size; ++i)
            d[i] = value;
    }

    // hashing
public:
    /// Structural, order-dependent hash over the live elements.
    [[nodiscard]] friend u64 hash(fixed_vector const& v) { return cc::make_hash_range(v); }

    // implementation
private:
    /// Destroys elements [new_size, _size) and sets the size. Precondition: 0 <= new_size <= _size.
    void _shrink_to(isize new_size)
    {
        T* const d = data();
        for (isize i = new_size; i < _size; ++i)
            d[i].~T();
        _size = new_size;
    }

    // Uninitialized aligned storage for N elements; only [0, _size) are alive. reinterpret_cast in data()
    // is well-defined for the objects placement-new'd into it.
    alignas(T) unsigned char _storage[sizeof(T) * N];
    isize _size = 0;
};
