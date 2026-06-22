#pragma once

#include <clean-core/assert.hh>
#include <clean-core/fwd.hh>

#include <initializer_list>
#include <type_traits>
#include <utility>

// TODO:
// - reinterpret_as / as_bytes / as_mutable_bytes
// - subspan api (first + last as well)
// - size_bytes

/// Non-owning view over a contiguous sequence of T, similar to std::span.
/// Stores a pointer and runtime size.
/// Trivially copyable regardless of T's triviality.
/// Does not own the underlying memory; caller must ensure the referenced data outlives the span.
template <class T>
struct cc::span
{
    // construction
public:
    /// Default span is empty: data() == nullptr, size() == 0.
    constexpr span() = default;

    // keep triviality
    constexpr span(span const&) = default;
    constexpr span(span&&) = default;
    constexpr span& operator=(span const&) = default;
    constexpr span& operator=(span&&) = default;
    constexpr ~span() = default;

    /// Creates a span viewing [ptr, ptr+size).
    /// Precondition: size >= 0.
    constexpr explicit span(T* ptr, isize size) : _data(ptr), _size(size)
    {
        CC_ASSERT(size >= 0, "span size must be non-negative");
    }

    /// Creates a span viewing [begin, end).
    /// Precondition: begin <= end.
    constexpr explicit span(T* begin, T* end) : _data(begin), _size(end - begin)
    {
        CC_ASSERT(begin <= end, "invalid pointer range");
    }

    /// Creates a span from an initializer_list.
    /// Only available when T is const; allows calling foo({1, 2, 3}) for foo(span<int const>).
    /// WARNING: initializer_list temporaries are destroyed at the end of the full expression.
    /// Safe ONLY as an immediate function argument: foo({1, 2, 3}).
    /// NEVER assign to a variable: auto s = span<int const>{1, 2, 3}; // DANGLING!
    constexpr span(std::initializer_list<std::remove_const_t<T>> init)
        requires std::is_const_v<T>
      : _data(init.begin()), _size(static_cast<isize>(init.size()))
    {
    }

    /// Creates a span viewing the entire C array.
    /// Deduces size N from array type; implicit conversion allowed.
    template <std::size_t N>
    constexpr span(T (&arr)[N]) : _data(arr), _size(static_cast<isize>(N))
    {
    }

    /// Creates a span from any container providing .data() and .size().
    /// The span does not own the container; the container must outlive the span.
    /// Passing a temporary container is safe when the span is used immediately (e.g., function argument).
    template <class Container>
        requires requires(Container&& c) {
            { c.data() } -> std::convertible_to<T*>;
            { c.size() } -> std::convertible_to<isize>;
        }
    constexpr span(Container&& c) : _data(c.data()), _size(static_cast<isize>(c.size()))
    {
    }

    // element access
public:
    /// Returns a reference to the element at index i.
    /// Precondition: 0 <= i < size().
    [[nodiscard]] constexpr T& operator[](isize i) const
    {
        CC_ASSERT(0 <= i && i < _size, "index out of bounds");
        return _data[i];
    }

    /// Returns a reference to the first element.
    /// Precondition: !empty().
    [[nodiscard]] constexpr T& front() const
    {
        CC_ASSERT(_size > 0, "front() called on empty span");
        return _data[0];
    }

    /// Returns a reference to the last element.
    /// Precondition: !empty().
    [[nodiscard]] constexpr T& back() const
    {
        CC_ASSERT(_size > 0, "back() called on empty span");
        return _data[_size - 1];
    }

    /// Returns a pointer to the underlying contiguous storage.
    /// May be nullptr if the span is default-constructed or empty.
    [[nodiscard]] constexpr T* data() const { return _data; }

    // iterators
public:
    /// Returns a pointer to the first element; nullptr if empty.
    /// Enables range-based for loops.
    [[nodiscard]] constexpr T* begin() const { return _data; }
    /// Returns a pointer to one past the last element.
    [[nodiscard]] constexpr T* end() const { return _data + _size; }

    // queries
public:
    /// Returns the number of elements in the span.
    [[nodiscard]] constexpr isize size() const { return _size; }
    /// Returns true if size() == 0.
    [[nodiscard]] constexpr bool empty() const { return _size == 0; }

    // members
private:
    T* _data = nullptr;
    isize _size = 0;
};

/// Non-owning view over a contiguous sequence of exactly N elements of type T.
/// Similar to span but with compile-time fixed size N.
/// Trivially copyable regardless of T's triviality.
/// Does not own the underlying memory; caller must ensure the referenced data outlives the span.
/// Supports tuple protocol for structured bindings and std::get.
template <class T, cc::isize N>
struct cc::fixed_span
{
    static_assert(N >= 0, "fixed_span size must be non-negative");

    // construction
public:
    /// Default constructor only available when N == 0; otherwise use a data source.
    constexpr fixed_span()
        requires(N == 0)
    = default;
    /// nullptr fixed_span is unreasonable; N elements cannot exist at nullptr.
    constexpr fixed_span(nullptr_t) = delete;

    // keep triviality
    constexpr fixed_span(fixed_span const&) = default;
    constexpr fixed_span(fixed_span&&) = default;
    constexpr fixed_span& operator=(fixed_span const&) = default;
    constexpr fixed_span& operator=(fixed_span&&) = default;
    constexpr ~fixed_span() = default;

    /// Creates a fixed_span viewing [ptr, ptr+N).
    /// The caller must ensure ptr points to at least N contiguous elements.
    constexpr explicit fixed_span(T* ptr) : _data(ptr) {}

    /// Creates a fixed_span viewing [ptr, ptr+N).
    /// Precondition: size == N.
    constexpr explicit fixed_span(T* ptr, isize size) : _data(ptr) { CC_ASSERT(size == N, "fixed_span size mismatch"); }

    /// Creates a fixed_span viewing [begin, end).
    /// Precondition: end - begin == N.
    constexpr explicit fixed_span(T* begin, T* end) : _data(begin)
    {
        CC_ASSERT(end - begin == N, "invalid pointer range for fixed_span");
    }

    /// Creates a fixed_span from an initializer_list.
    /// Only available when T is const.
    /// Precondition: init.size() == N.
    /// WARNING: initializer_list temporaries are destroyed at the end of the full expression.
    /// Safe ONLY as an immediate function argument: foo({1, 2, 3}).
    /// NEVER assign to a variable: auto s = fixed_span<int const, 3>{1, 2, 3}; // DANGLING!
    constexpr explicit fixed_span(std::initializer_list<std::remove_const_t<T>> init)
        requires std::is_const_v<T>
      : _data(init.begin())
    {
        CC_ASSERT(static_cast<isize>(init.size()) == N, "initializer_list size mismatch");
    }

    /// Creates a fixed_span viewing the entire C array.
    /// Static assertion ensures M == N.
    template <std::size_t M>
    constexpr fixed_span(T (&arr)[M]) : _data(arr)
    {
        static_assert(M == static_cast<std::size_t>(N), "C array size must match fixed_span size");
    }

    /// Creates a fixed_span from any container providing .data() and .size().
    /// The span does not own the container; the container must outlive the span.
    /// Precondition: c.size() == N.
    /// Passing a temporary container is safe when the span is used immediately (e.g., function argument).
    template <class Container>
        requires requires(Container&& c) {
            { c.data() } -> std::convertible_to<T*>;
            { c.size() } -> std::convertible_to<isize>;
        }
    constexpr explicit fixed_span(Container&& c) : _data(c.data())
    {
        CC_ASSERT(static_cast<isize>(c.size()) == N, "container size mismatch");
    }

    // element access
public:
    /// Returns a reference to the element at index i.
    /// Precondition: 0 <= i < N.
    [[nodiscard]] constexpr T& operator[](isize i) const
    {
        CC_ASSERT(0 <= i && i < N, "index out of bounds");
        return _data[i];
    }

    /// Returns a reference to the first element.
    /// Requires N > 0 (compile-time check).
    [[nodiscard]] constexpr T& front() const
    {
        static_assert(N > 0, "front() not available on empty fixed_span");
        return _data[0];
    }

    /// Returns a reference to the last element.
    /// Requires N > 0 (compile-time check).
    [[nodiscard]] constexpr T& back() const
    {
        static_assert(N > 0, "back() not available on empty fixed_span");
        return _data[N - 1];
    }

    /// Returns a pointer to the underlying contiguous storage.
    /// May be nullptr if default-constructed.
    [[nodiscard]] constexpr T* data() const { return _data; }

    // iterators
public:
    /// Returns a pointer to the first element; nullptr if default-constructed.
    /// Enables range-based for loops.
    [[nodiscard]] constexpr T* begin() const { return _data; }
    /// Returns a pointer to one past the last element.
    [[nodiscard]] constexpr T* end() const { return _data + N; }

    // queries
public:
    /// Returns the compile-time size N.
    [[nodiscard]] constexpr isize size() const { return N; }
    /// Returns true if N == 0 (compile-time constant).
    [[nodiscard]] constexpr bool empty() const { return N == 0; }

    // tuple protocol
public:
    /// Returns a reference to the I-th element.
    /// Supports std::get<I>(span) and structured bindings.
    /// Requires 0 <= I < N (compile-time check).
    template <isize I>
    [[nodiscard]] constexpr T& get() const
    {
        static_assert(0 <= I && I < N, "index out of bounds");
        return _data[I];
    }

    // members
private:
    T* _data = nullptr;
};

/// Specialization of std::tuple_size for fixed_span to enable structured bindings.
template <class T, cc::isize N>
struct std::tuple_size<cc::fixed_span<T, N>> : std::integral_constant<std::size_t, static_cast<std::size_t>(N)>
{
};

/// Specialization of std::tuple_element for fixed_span to enable structured bindings.
/// All elements have type T.
template <std::size_t I, class T, cc::isize N>
struct std::tuple_element<I, cc::fixed_span<T, N>>
{
    using type = T;
};
