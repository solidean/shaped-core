#pragma once

#include <clean-core/impl/allocating_container.hh>

#include <utility>


// TODO:
// - equality, order, hashing


/// Fixed-size array of exactly N elements of type T.
/// Similar to std::array but follows clean-core conventions.
/// Trivial aggregate type - supports aggregate initialization: fixed_array<int, 3> arr = {1, 2, 3}.
/// Owns the underlying memory.
template <class T, cc::isize N>
struct cc::fixed_array
{
    static_assert(N >= 0, "fixed_array size must be non-negative");

    // members
public:
    T _data[N];

    // element access
public:
    /// Returns a reference to the element at index i.
    /// Precondition: 0 <= i < N.
    [[nodiscard]] constexpr T& operator[](isize i)
    {
        CC_ASSERT(0 <= i && i < N, "index out of bounds");
        return _data[i];
    }
    [[nodiscard]] constexpr T const& operator[](isize i) const
    {
        CC_ASSERT(0 <= i && i < N, "index out of bounds");
        return _data[i];
    }

    /// Returns a reference to the first element.
    [[nodiscard]] constexpr T& front() { return _data[0]; }
    [[nodiscard]] constexpr T const& front() const { return _data[0]; }

    /// Returns a reference to the last element.
    [[nodiscard]] constexpr T& back() { return _data[N - 1]; }
    [[nodiscard]] constexpr T const& back() const { return _data[N - 1]; }

    /// Returns a pointer to the underlying contiguous storage.
    [[nodiscard]] constexpr T* data() { return _data; }
    [[nodiscard]] constexpr T const* data() const { return _data; }

    // iterators
public:
    /// Returns a pointer to the first element.
    /// Enables range-based for loops.
    [[nodiscard]] constexpr T* begin() { return _data; }
    [[nodiscard]] constexpr T* end() { return _data + N; }
    [[nodiscard]] constexpr T const* begin() const { return _data; }
    [[nodiscard]] constexpr T const* end() const { return _data + N; }

    // queries
public:
    /// Returns the compile-time size N.
    [[nodiscard]] constexpr isize size() const { return N; }
    /// Returns true if N == 0 (compile-time constant).
    [[nodiscard]] constexpr bool empty() const { return N == 0; }

    // tuple protocol
public:
    /// Returns a reference to the I-th element.
    /// Supports std::get<I>(arr) and structured bindings.
    /// Requires 0 <= I < N (compile-time check).
    template <isize I>
    [[nodiscard]] constexpr T& get()
    {
        static_assert(0 <= I && I < N, "index out of bounds");
        return _data[I];
    }
    template <isize I>
    [[nodiscard]] constexpr T const& get() const
    {
        static_assert(0 <= I && I < N, "index out of bounds");
        return _data[I];
    }
};

/// Specialization for N == 0 (empty array).
/// Zero-sized arrays T[0] are not valid in standard C++.
template <class T>
struct cc::fixed_array<T, 0>
{
    // element access
public:
    /// operator[] not callable on empty array.
    [[nodiscard]] constexpr T& operator[](isize)
    {
        static_assert(false, "operator[] not available on empty fixed_array");
    }
    [[nodiscard]] constexpr T const& operator[](isize) const
    {
        static_assert(false, "operator[] not available on empty fixed_array");
    }

    /// front() not callable on empty array.
    [[nodiscard]] constexpr T& front() { static_assert(false, "front() not available on empty fixed_array"); }
    [[nodiscard]] constexpr T const& front() const
    {
        static_assert(false, "front() not available on empty fixed_array");
    }

    /// back() not callable on empty array.
    [[nodiscard]] constexpr T& back() { static_assert(false, "back() not available on empty fixed_array"); }
    [[nodiscard]] constexpr T const& back() const { static_assert(false, "back() not available on empty fixed_array"); }

    /// Returns nullptr for empty array.
    [[nodiscard]] constexpr T* data() { return nullptr; }
    [[nodiscard]] constexpr T const* data() const { return nullptr; }

    // iterators
public:
    /// Returns nullptr (begin == end for empty array).
    [[nodiscard]] constexpr T* begin() { return nullptr; }
    [[nodiscard]] constexpr T* end() { return nullptr; }
    [[nodiscard]] constexpr T const* begin() const { return nullptr; }
    [[nodiscard]] constexpr T const* end() const { return nullptr; }

    // queries
public:
    /// Returns 0.
    [[nodiscard]] constexpr isize size() const { return 0; }
    /// Returns true.
    [[nodiscard]] constexpr bool empty() const { return true; }
};

/// Specialization of std::tuple_size for fixed_array to enable structured bindings.
template <class T, cc::isize N>
struct std::tuple_size<cc::fixed_array<T, N>> : std::integral_constant<std::size_t, static_cast<std::size_t>(N)>
{
};

/// Specialization of std::tuple_element for fixed_array to enable structured bindings.
/// All elements have type T.
template <std::size_t I, class T, cc::isize N>
struct std::tuple_element<I, cc::fixed_array<T, N>>
{
    using type = T;
};
