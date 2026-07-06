#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/hash.hh> // cc::make_hash_range
#include <clean-core/common/utility.hh>
#include <clean-core/container/impl/allocating_container.hh>
#include <clean-core/error/optional.hh>
#include <clean-core/fwd.hh>

#include <initializer_list>
#include <new>

/// Growable vector with small-vector optimization: the first `N` elements live inline (no allocation),
/// spilling to the heap only once the size exceeds `N`. Ideal where the common case holds a handful of
/// elements but an occasional overflow must still be handled correctly.
///
/// Storage mirrors `cc::string`'s SSO: a union of an inline buffer (`data_small`) and a heap
/// representation (`data_heap`, a `cc::allocating_container<T>`), so the heap side reuses `cc::allocation`'s
/// growth strategy, cache-line alignment, memory-resource support, and exception guarantees. The public
/// surface mirrors `cc::vector` (create_* factories, resize_* family, pop_back/remove_back,
/// extract_allocation) so it feels the same. `is_inline()` reports whether storage is still the inline
/// buffer. Value semantics (deep copy); a moved-from small_vector is left empty and inline.
///
/// Usage:
///   cc::small_vector<int, 4> v; // holds up to 4 ints without allocating
///   v.push_back(1);
///   v.emplace_back(2);
///   for (int x : v) { ... }
template <class T, cc::isize N>
struct cc::small_vector
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>, "small_vector needs non-const object elements");
    static_assert(N >= 1, "inline capacity N must be >= 1");

    // factories (mirroring cc::vector)
public:
    /// Empty, but with a specific memory resource used once it spills to the heap.
    [[nodiscard]] static small_vector create_with_resource(cc::memory_resource const* resource)
    {
        small_vector v;
        v._resource = resource;
        return v;
    }

    /// Adopts an existing allocation — always heap mode, even if its content would fit inline (mirrors
    /// cc::vector / cc::string). Its live objects become the elements; its resource becomes sticky.
    [[nodiscard]] static small_vector create_from_allocation(cc::allocation<T> data)
    {
        small_vector v;
        v._resource = data.custom_resource;
        v._destroy();
        new (&v._s.heap) data_heap(data_heap::create_from_allocation(cc::move(data)));
        v._is_small = false;
        return v;
    }

    /// `size` default-constructed (value-initialized) elements.
    [[nodiscard]] static small_vector create_defaulted(isize size, cc::memory_resource const* resource = nullptr)
    {
        small_vector v = create_with_resource(resource);
        v.resize_to_defaulted(size);
        return v;
    }

    /// `size` elements, each a copy of `value`.
    [[nodiscard]] static small_vector create_filled(isize size,
                                                    T const& value,
                                                    cc::memory_resource const* resource = nullptr)
    {
        small_vector v = create_with_resource(resource);
        v.resize_to_filled(size, value);
        return v;
    }

    /// `size` uninitialized elements (trivial types only) — the caller must fill them before reading.
    [[nodiscard]] static small_vector create_uninitialized(isize size, cc::memory_resource const* resource = nullptr)
    {
        small_vector v = create_with_resource(resource);
        v.resize_to_uninitialized(size);
        return v;
    }

    /// A deep copy of `source`.
    [[nodiscard]] static small_vector create_copy_of(cc::span<T const> source,
                                                     cc::memory_resource const* resource = nullptr)
    {
        small_vector v = create_with_resource(resource);
        v.reserve(source.size());
        for (auto const& e : source)
            v.push_back(e);
        return v;
    }

    /// Empty, with capacity reserved for at least `capacity` elements without reallocation.
    [[nodiscard]] static small_vector create_with_capacity(isize capacity, cc::memory_resource const* resource = nullptr)
    {
        small_vector v = create_with_resource(resource);
        v.reserve(capacity);
        return v;
    }

    // ctors / dtor / assignment
public:
    small_vector()
    {
        _is_small = true;
        _s.sso.size = 0;
    }

    small_vector(std::initializer_list<T> init)
    {
        _is_small = true;
        _s.sso.size = 0;
        reserve(isize(init.size()));
        for (auto const& e : init)
            push_back(e);
    }

    small_vector(small_vector const& rhs) { _init_copy(rhs); }
    small_vector(small_vector&& rhs) noexcept { _init_move(cc::move(rhs)); }

    small_vector& operator=(small_vector const& rhs)
    {
        if (this != &rhs)
        {
            _destroy();
            _init_copy(rhs);
        }
        return *this;
    }

    small_vector& operator=(small_vector&& rhs) noexcept
    {
        if (this != &rhs)
        {
            _destroy();
            _init_move(cc::move(rhs));
        }
        return *this;
    }

    ~small_vector() { _destroy(); }

    // element access
public:
    [[nodiscard]] T& operator[](isize i)
    {
        CC_ASSERT(i >= 0 && i < size(), "small_vector index out of bounds");
        return data()[i];
    }
    [[nodiscard]] T const& operator[](isize i) const
    {
        CC_ASSERT(i >= 0 && i < size(), "small_vector index out of bounds");
        return data()[i];
    }

    [[nodiscard]] T& front()
    {
        CC_ASSERT(!empty(), "front() on empty small_vector");
        return data()[0];
    }
    [[nodiscard]] T const& front() const
    {
        CC_ASSERT(!empty(), "front() on empty small_vector");
        return data()[0];
    }
    [[nodiscard]] T& back()
    {
        CC_ASSERT(!empty(), "back() on empty small_vector");
        return data()[size() - 1];
    }
    [[nodiscard]] T const& back() const
    {
        CC_ASSERT(!empty(), "back() on empty small_vector");
        return data()[size() - 1];
    }

    [[nodiscard]] T* data() { return _is_small ? _s.sso.ptr() : _s.heap.data(); }
    [[nodiscard]] T const* data() const { return _is_small ? _s.sso.ptr() : _s.heap.data(); }

    // iterators
public:
    [[nodiscard]] T* begin() { return data(); }
    [[nodiscard]] T* end() { return data() + size(); }
    [[nodiscard]] T const* begin() const { return data(); }
    [[nodiscard]] T const* end() const { return data() + size(); }

    // queries
public:
    [[nodiscard]] isize size() const { return _is_small ? _s.sso.size : _s.heap.size(); }
    [[nodiscard]] bool empty() const { return size() == 0; }
    [[nodiscard]] isize size_bytes() const { return size() * isize(sizeof(T)); }
    [[nodiscard]] isize capacity() const { return _is_small ? N : _s.heap.size() + _s.heap.capacity_back(); }
    [[nodiscard]] isize capacity_back() const { return capacity() - size(); }
    [[nodiscard]] bool has_capacity_back_for(isize count) const { return capacity_back() >= count; }

    /// Compile-time inline capacity (elements storable without any allocation).
    [[nodiscard]] static constexpr isize inline_capacity() { return N; }

    /// Whether the current storage is the inline buffer (i.e. no heap allocation is held).
    [[nodiscard]] bool is_inline() const { return _is_small; }

    // appending / removing
public:
    void push_back(T const& value) { emplace_back(value); }
    void push_back(T&& value) { emplace_back(cc::move(value)); }

    template <class... Args>
    T& emplace_back(Args&&... args)
    {
        if (_is_small)
        {
            if (_s.sso.size < N)
            {
                T* const slot = _s.sso.ptr() + _s.sso.size;
                new (cc::placement_new, slot) T(cc::forward<Args>(args)...);
                ++_s.sso.size;
                return *slot;
            }
            _spill_to_heap(_s.sso.size + 1);
        }
        return _s.heap.emplace_back(cc::forward<Args>(args)...);
    }

    /// Removes and returns the last element. Precondition: !empty().
    [[nodiscard]] T pop_back()
    {
        CC_ASSERT(!empty(), "pop_back() on empty small_vector");
        T value = cc::move(back());
        remove_back();
        return value;
    }

    /// Removes the last element without returning it. Precondition: !empty().
    void remove_back()
    {
        CC_ASSERT(!empty(), "remove_back() on empty small_vector");
        if (_is_small)
        {
            --_s.sso.size;
            _s.sso.ptr()[_s.sso.size].~T();
        }
        else
            _s.heap.remove_back();
    }

    /// Constructs an element at the back without reallocation — the existing capacity must suffice.
    template <class... Args>
    T& emplace_back_stable(Args&&... args)
    {
        CC_ASSERT(has_capacity_back_for(1), "emplace_back_stable requires spare capacity (would reallocate)");
        if (_is_small)
        {
            T* const slot = _s.sso.ptr() + _s.sso.size;
            new (cc::placement_new, slot) T(cc::forward<Args>(args)...);
            ++_s.sso.size;
            return *slot;
        }
        return _s.heap.emplace_back_stable(cc::forward<Args>(args)...);
    }
    T& push_back_stable(T const& value) { return emplace_back_stable(value); }
    T& push_back_stable(T&& value) { return emplace_back_stable(cc::move(value)); }

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
        CC_ASSERT(idx >= 0 && idx < size(), "remove_at index out of bounds");
        T* const d = data();
        cc::impl::compact_move_objects_backward(d + idx, d + idx + 1, d + size());
        _shrink_to(size() - 1);
    }
    /// Removes and returns the element at `idx` by swapping in the last element (O(1), does not preserve order).
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
        isize const last = size() - 1;
        if (idx != last)
            data()[idx] = cc::move(data()[last]);
        _shrink_to(last);
    }

    /// Removes `count` elements starting at `start`, preserving order. Precondition: start + count <= size().
    void remove_at_range(isize start, isize count)
    {
        CC_ASSERT(start >= 0 && count >= 0 && start + count <= size(), "remove_at_range out of bounds");
        if (count == 0)
            return;
        T* const d = data();
        cc::impl::compact_move_objects_backward(d + start, d + start + count, d + size());
        _shrink_to(size() - count);
    }
    /// Removes `count` elements starting at `start` by moving trailing elements into the gap (unordered).
    void remove_at_range_unordered(isize start, isize count)
    {
        CC_ASSERT(start >= 0 && count >= 0 && start + count <= size(), "remove_at_range_unordered out of bounds");
        if (count == 0)
            return;
        T* const d = data();
        cc::impl::compact_move_objects_backward(d + start, d + size() - count, d + size());
        _shrink_to(size() - count);
    }
    /// Removes the range [start, end), preserving order. Precondition: start <= end <= size().
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

    /// Removes every element for which `pred` is true (preserving order); returns the number removed.
    template <class Pred>
    isize remove_all_where(Pred&& pred)
    {
        T* const d = data();
        isize const n = size();
        isize write = 0;
        for (isize read = 0; read < n; ++read)
            if (!pred(d[read]))
            {
                if (write != read)
                    d[write] = cc::move(d[read]);
                ++write;
            }
        _shrink_to(write);
        return n - write;
    }
    /// Removes the first element matching `pred`; returns its index, or nullopt if none matched.
    template <class Pred>
    cc::optional<isize> remove_first_where(Pred&& pred)
    {
        for (isize i = 0, n = size(); i < n; ++i)
            if (pred((*this)[i]))
            {
                remove_at(i);
                return i;
            }
        return {};
    }
    /// Removes the last element matching `pred`; returns its index, or nullopt if none matched.
    template <class Pred>
    cc::optional<isize> remove_last_where(Pred&& pred)
    {
        for (isize i = size() - 1; i >= 0; --i)
            if (pred((*this)[i]))
            {
                remove_at(i);
                return i;
            }
        return {};
    }
    /// Removes every element equal to `value`; returns the number removed.
    isize remove_all_value(T const& value)
    {
        return remove_all_where([&](T const& e) { return e == value; });
    }
    /// Removes the first element equal to `value`; returns its index, or nullopt.
    cc::optional<isize> remove_first_value(T const& value)
    {
        return remove_first_where([&](T const& e) { return e == value; });
    }
    /// Removes the last element equal to `value`; returns its index, or nullopt.
    cc::optional<isize> remove_last_value(T const& value)
    {
        return remove_last_where([&](T const& e) { return e == value; });
    }
    /// Keeps only the elements for which `pred` is true (preserving order); returns the number removed.
    template <class Pred>
    isize retain_all_where(Pred&& pred)
    {
        return remove_all_where([&](T const& e) { return !pred(e); });
    }

    /// Destroys all elements; size becomes 0. Keeps the current storage mode and capacity.
    void clear()
    {
        if (_is_small)
        {
            cc::impl::destroy_objects_in_reverse(_s.sso.ptr(), _s.sso.ptr() + _s.sso.size);
            _s.sso.size = 0;
        }
        else
            _s.heap.clear();
    }

    /// Assigns `value` to every existing element.
    void fill(T const& value)
    {
        for (isize i = 0, n = size(); i < n; ++i)
            data()[i] = value;
    }

    // capacity / sizing (mirroring cc::vector)
public:
    /// Ensures storage for at least `count` elements without further reallocation (exponential growth).
    void reserve(isize count)
    {
        if (capacity() >= count)
            return;
        if (_is_small)
            _spill_to_heap(count);
        else
            _s.heap.reserve_back(count - _s.heap.size());
    }

    /// Ensures storage for at least `count` elements, allocating exactly (no exponential slack).
    void reserve_exact(isize count)
    {
        if (capacity() >= count)
            return;
        if (_is_small)
            _spill_to_heap(count, /*exact*/ true);
        else
            _s.heap.reserve_back_exact(count - _s.heap.size());
    }

    /// Shrinks to `new_size` by destroying trailing elements. Precondition: new_size <= size().
    void resize_down_to(isize new_size)
    {
        CC_ASSERT(new_size >= 0 && new_size <= size(), "resize_down_to: new_size must be in [0, size()]");
        _shrink_to(new_size);
    }

    /// Resizes to `new_size`; new elements are value-initialized.
    void resize_to_defaulted(isize new_size)
    {
        CC_ASSERT(new_size >= 0, "small_vector size must be >= 0");
        if (new_size <= size())
            return _shrink_to(new_size);
        reserve(new_size);
        if (_is_small)
        {
            T* p = _s.sso.ptr() + _s.sso.size;
            cc::impl::default_create_objects_to(p, new_size - _s.sso.size);
            _s.sso.size = new_size;
        }
        else
            _s.heap.resize_to_defaulted(new_size);
    }

    /// Resizes to `new_size`; new elements are copies of `value`.
    void resize_to_filled(isize new_size, T const& value)
    {
        CC_ASSERT(new_size >= 0, "small_vector size must be >= 0");
        if (new_size <= size())
            return _shrink_to(new_size);
        reserve(new_size);
        if (_is_small)
        {
            T* p = _s.sso.ptr() + _s.sso.size;
            cc::impl::fill_create_objects_to(p, new_size - _s.sso.size, value);
            _s.sso.size = new_size;
        }
        else
            _s.heap.resize_to_filled(new_size, value);
    }

    /// Resizes to `new_size`; new elements are left uninitialized (trivial types only).
    void resize_to_uninitialized(isize new_size)
    {
        static_assert(std::is_trivially_copyable_v<T>, "resize_to_uninitialized requires a trivially copyable T");
        CC_ASSERT(new_size >= 0, "small_vector size must be >= 0");
        if (new_size <= size())
            return _shrink_to(new_size);
        reserve(new_size);
        if (_is_small)
            _s.sso.size = new_size; // new elements intentionally uninitialized
        else
            _s.heap.resize_to_uninitialized(new_size);
    }

    /// Resizes to `new_size`; new elements are constructed with `args...`.
    template <class... Args>
    void resize_to_constructed(isize new_size, Args&&... args)
    {
        CC_ASSERT(new_size >= 0, "small_vector size must be >= 0");
        if (new_size <= size())
            return _shrink_to(new_size);
        reserve(new_size);
        while (size() < new_size)
            emplace_back_stable(args...); // args reused (copied) for each new element
    }

    /// Clears, then resizes to `new_size` with value-initialized elements.
    void clear_resize_to_defaulted(isize new_size)
    {
        clear();
        resize_to_defaulted(new_size);
    }
    /// Clears, then resizes to `new_size` with copies of `value`.
    void clear_resize_to_filled(isize new_size, T const& value)
    {
        clear();
        resize_to_filled(new_size, value);
    }
    /// Clears, then resizes to `new_size` with uninitialized elements (trivial types only).
    void clear_resize_to_uninitialized(isize new_size)
    {
        clear();
        resize_to_uninitialized(new_size);
    }
    /// Clears, then resizes to `new_size` with elements constructed from `args...`.
    template <class... Args>
    void clear_resize_to_constructed(isize new_size, Args&&... args)
    {
        clear();
        resize_to_constructed(new_size, cc::forward<Args>(args)...);
    }

    /// Ensures capacity for `count` more elements beyond the current size (exponential growth).
    void reserve_back(isize count) { reserve(size() + count); }
    /// Ensures capacity for `count` more elements beyond the current size (exact allocation).
    void reserve_back_exact(isize count) { reserve_exact(size() + count); }

    /// Reduces capacity toward the current size. If the elements fit inline, returns to inline storage
    /// (freeing the heap allocation); otherwise shrinks the heap allocation.
    void shrink_to_fit()
    {
        if (_is_small)
            return; // inline storage is already minimal
        if (size() <= N)
        {
            // Re-inline: extract the allocation, move its elements into the inline buffer, then let the
            // extracted allocation destroy the moved-from originals and free its storage.
            cc::allocation<T> alloc = _s.heap.extract_allocation();
            _s.heap.~data_heap();
            _is_small = true;
            _s.sso.size = 0;
            T* dst = _s.sso.ptr();
            cc::impl::move_create_objects_to(dst, alloc.obj_start, alloc.obj_end);
            _s.sso.size = alloc.obj_end - alloc.obj_start;
        }
        else
            _s.heap.shrink_to_fit();
    }

    // allocation extraction
public:
    /// Extracts the underlying allocation, leaving this empty. If currently inline, the elements are first
    /// materialized into a fresh heap allocation. Mirrors cc::vector::extract_allocation.
    [[nodiscard]] cc::allocation<T> extract_allocation()
    {
        if (_is_small)
            _spill_to_heap(_s.sso.size, /*exact*/ true);
        cc::allocation<T> out = _s.heap.extract_allocation();
        _s.heap.~data_heap();
        _is_small = true;
        _s.sso.size = 0;
        return out;
    }

    /// Extracts the underlying allocation only if one is currently held (heap mode); returns nullopt while
    /// inline (nothing is allocated to hand out). Leaves this empty on success.
    [[nodiscard]] cc::optional<cc::allocation<T>> try_extract_allocation()
    {
        if (_is_small)
            return {};
        cc::allocation<T> out = _s.heap.extract_allocation();
        _s.heap.~data_heap();
        _is_small = true;
        _s.sso.size = 0;
        return out;
    }

    /// Structural, order-dependent hash over the elements (independent of capacity / storage mode).
    [[nodiscard]] friend u64 hash(small_vector const& v) { return cc::make_hash_range(v); }

private:
    // Heap representation: a back-growing allocating_container over T (vector-like policy).
    struct data_heap : cc::allocating_container<T, data_heap>
    {
        static constexpr bool uses_capacity_front = false;
    };

    // Inline representation: a runtime size plus raw storage for N elements.
    struct data_small
    {
        isize size;
        alignas(T) cc::byte storage[N * sizeof(T)];

        [[nodiscard]] T* ptr() { return reinterpret_cast<T*>(storage); }
        [[nodiscard]] T const* ptr() const { return reinterpret_cast<T const*>(storage); }
    };

    // Move the inline elements into a fresh heap allocation of at least `min_capacity`, then switch modes.
    // Precondition: currently small. `exact` uses exact (non-exponential) allocation.
    void _spill_to_heap(isize min_capacity, bool exact = false)
    {
        data_heap heap = data_heap::create_with_resource(_resource);
        isize const want = min_capacity > _s.sso.size ? min_capacity : _s.sso.size;
        if (exact)
            heap.reserve_back_exact(want);
        else
            heap.reserve_back(want);
        for (isize i = 0; i < _s.sso.size; ++i)
            heap.emplace_back_stable(cc::move(_s.sso.ptr()[i]));
        cc::impl::destroy_objects_in_reverse(_s.sso.ptr(), _s.sso.ptr() + _s.sso.size);

        new (&_s.heap) data_heap(cc::move(heap));
        _is_small = false;
    }

    // Shrink to `n` (n <= size()), destroying the trailing elements.
    void _shrink_to(isize n)
    {
        if (_is_small)
        {
            cc::impl::destroy_objects_in_reverse(_s.sso.ptr() + n, _s.sso.ptr() + _s.sso.size);
            _s.sso.size = n;
        }
        else
            _s.heap.resize_down_to(n);
    }

    // Destroy the active representation (elements + any heap allocation), leaving the union inactive.
    void _destroy()
    {
        if (_is_small)
            cc::impl::destroy_objects_in_reverse(_s.sso.ptr(), _s.sso.ptr() + _s.sso.size);
        else
            _s.heap.~data_heap();
    }

    // Deep-copy rhs into a fresh (uninitialized) *this. Picks inline storage when the content fits.
    void _init_copy(small_vector const& rhs)
    {
        _resource = rhs._resource;
        if (rhs.size() <= N)
        {
            _is_small = true;
            _s.sso.size = 0;
            T* dst = _s.sso.ptr();
            cc::impl::copy_create_objects_to(dst, rhs.data(), rhs.data() + rhs.size());
            _s.sso.size = rhs.size();
        }
        else
        {
            _is_small = false;
            new (&_s.heap) data_heap(data_heap::create_with_resource(_resource));
            _s.heap.reserve_back(rhs.size());
            for (isize i = 0; i < rhs.size(); ++i)
                _s.heap.push_back_stable(rhs[i]);
        }
    }

    // Take rhs's contents into a fresh (uninitialized) *this, leaving rhs empty and inline.
    void _init_move(small_vector&& rhs) noexcept
    {
        _resource = rhs._resource;
        if (rhs._is_small)
        {
            _is_small = true;
            _s.sso.size = 0;
            T* dst = _s.sso.ptr();
            cc::impl::move_create_objects_to(dst, rhs._s.sso.ptr(), rhs._s.sso.ptr() + rhs._s.sso.size);
            _s.sso.size = rhs._s.sso.size;
            cc::impl::destroy_objects_in_reverse(rhs._s.sso.ptr(), rhs._s.sso.ptr() + rhs._s.sso.size);
            rhs._s.sso.size = 0;
        }
        else
        {
            // Steal the heap allocation, then reset rhs to an empty inline vector.
            _is_small = false;
            new (&_s.heap) data_heap(cc::move(rhs._s.heap));
            rhs._s.heap.~data_heap();
            rhs._is_small = true;
            rhs._s.sso.size = 0;
        }
    }

    union storage_t
    {
        data_heap heap;
        data_small sso;

        storage_t() {}  // NOLINT — the small_vector picks and manages the active member
        ~storage_t() {} // NOLINT — destruction is driven by _destroy()
    } _s;
    bool _is_small = true;
    cc::memory_resource const* _resource = nullptr; // sticky resource, used once storage spills to the heap
};
