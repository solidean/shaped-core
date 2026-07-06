#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/common/utility.hh>
#include <clean-core/container/impl/allocating_container.hh>
#include <clean-core/fwd.hh>

/// Growable vector with small-vector optimization: the first `N` elements live inline (no allocation),
/// spilling to the heap only once the size exceeds `N`. Ideal where the common case holds a handful of
/// elements but an occasional overflow must still be handled correctly.
///
/// Storage mirrors `cc::string`'s SSO: a union of an inline buffer (`data_small`) and a heap
/// representation (`data_heap`, a `cc::allocating_container<T>`), so the heap side reuses `cc::allocation`'s
/// growth strategy, cache-line alignment, memory-resource support, and exception guarantees. `is_inline()`
/// reports whether the current storage is the inline buffer — useful in tests to assert the no-allocation
/// fast path. Value semantics (deep copy); a moved-from small_vector is left empty and inline.
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

    // ctors / dtor / assignment
public:
    small_vector()
    {
        _is_small = true;
        _s.sso.size = 0;
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
    [[nodiscard]] isize capacity() const { return _is_small ? N : _s.heap.size() + _s.heap.capacity_back(); }

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

    void pop_back()
    {
        CC_ASSERT(!empty(), "pop_back() on empty small_vector");
        if (_is_small)
        {
            --_s.sso.size;
            _s.sso.ptr()[_s.sso.size].~T();
        }
        else
        {
            _s.heap.resize_down_to(_s.heap.size() - 1);
        }
    }

    void clear()
    {
        if (_is_small)
        {
            cc::impl::destroy_objects_in_reverse(_s.sso.ptr(), _s.sso.ptr() + _s.sso.size);
            _s.sso.size = 0;
        }
        else
        {
            _s.heap.clear();
        }
    }

    // capacity / sizing
public:
    /// Ensures storage for at least `count` elements without further reallocation.
    void reserve(isize count)
    {
        if (count <= N)
            return; // the inline buffer already covers it
        if (_is_small)
            _spill_to_heap(count);
        else if (count > _s.heap.size())
            _s.heap.reserve_back(count - _s.heap.size());
    }

    /// Grows or shrinks to `new_size`; new elements are value-initialized.
    void resize(isize new_size)
    {
        CC_ASSERT(new_size >= 0, "small_vector size must be >= 0");
        isize const cur = size();
        if (new_size < cur)
        {
            if (_is_small)
            {
                cc::impl::destroy_objects_in_reverse(_s.sso.ptr() + new_size, _s.sso.ptr() + cur);
                _s.sso.size = new_size;
            }
            else
                _s.heap.resize_down_to(new_size);
        }
        else if (new_size > cur)
        {
            if (_is_small && new_size <= N)
            {
                T* p = _s.sso.ptr() + cur;
                cc::impl::default_create_objects_to(p, new_size - cur);
                _s.sso.size = new_size;
            }
            else
            {
                if (_is_small)
                    _spill_to_heap(new_size);
                _s.heap.resize_to_defaulted(new_size);
            }
        }
    }

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
    // Precondition: currently small.
    void _spill_to_heap(isize min_capacity)
    {
        data_heap heap;
        isize const want = min_capacity > _s.sso.size ? min_capacity : _s.sso.size;
        heap.reserve_back(want);
        for (isize i = 0; i < _s.sso.size; ++i)
            heap.emplace_back_stable(cc::move(_s.sso.ptr()[i]));
        cc::impl::destroy_objects_in_reverse(_s.sso.ptr(), _s.sso.ptr() + _s.sso.size);

        new (&_s.heap) data_heap(cc::move(heap));
        _is_small = false;
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
            new (&_s.heap) data_heap();
            _s.heap.reserve_back(rhs.size());
            for (isize i = 0; i < rhs.size(); ++i)
                _s.heap.push_back_stable(rhs[i]);
        }
    }

    // Take rhs's contents into a fresh (uninitialized) *this, leaving rhs empty and inline.
    void _init_move(small_vector&& rhs) noexcept
    {
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
};
