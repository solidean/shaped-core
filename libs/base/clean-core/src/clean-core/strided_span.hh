#pragma once

#include <clean-core/assert.hh>
#include <clean-core/fwd.hh>
#include <clean-core/optional.hh>
#include <clean-core/span.hh>
#include <clean-core/utility.hh>

#include <initializer_list>
#include <type_traits>

/// Forward iterator for non-contiguous data with a constant stride.
///
/// This iterator allows iteration over elements of type T that are stored with a
/// fixed byte offset (stride) between consecutive elements.
///
/// The stride represents the byte offset between consecutive elements and can be:
/// - positive: forward iteration through memory
/// - negative: backward iteration through memory
/// - zero: all elements alias the same memory location (repeated element access)
///
/// IMPORTANT: The stride must ensure that all accessed elements are properly aligned
/// for type T. Misaligned accesses lead to undefined behavior.
///
/// DESIGN: This iterator uses a counting approach (ptr, stride, remaining_count).
/// It compares against cc::sentinel for end-of-range detection, properly supporting
/// zero-stride where the pointer never advances.
///
/// NOTE: This is a forward iterator only - supports range-based for, but not random access.
/// Use operator[] on strided_span for indexed access.
template <class T>
struct cc::strided_iterator
{
    using byte_ptr = std::conditional_t<std::is_const_v<T>, cc::byte const*, cc::byte*>;

    constexpr strided_iterator() = default;
    constexpr strided_iterator(byte_ptr ptr, isize stride, isize size) // NOLINT(bugprone-easily-swappable-parameters)
      : _ptr(ptr), _stride_bytes(stride), _remaining_count(size)
    {
        CC_ASSERT(size >= 0, "strided_iterator size must be non-negative");
    }

    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    [[nodiscard]] constexpr T& operator*() const
    {
        CC_ASSERT(_remaining_count > 0, "dereferencing past-the-end iterator");
        return *reinterpret_cast<T*>(_ptr);
    }
    [[nodiscard]] constexpr T* operator->() const
    {
        CC_ASSERT(_remaining_count > 0, "dereferencing past-the-end iterator");
        return reinterpret_cast<T*>(_ptr);
    }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

    constexpr strided_iterator& operator++()
    {
        CC_ASSERT(_remaining_count > 0, "incrementing past-the-end iterator");
        _ptr += _stride_bytes;
        --_remaining_count;
        return *this;
    }

    // Comparison with sentinel (for range-based for loops)
    [[nodiscard]] friend constexpr bool operator==(strided_iterator const& it, cc::sentinel)
    {
        return it._remaining_count == 0;
    }
    [[nodiscard]] friend constexpr bool operator!=(strided_iterator const& it, cc::sentinel)
    {
        return it._remaining_count != 0;
    }

private:
    byte_ptr _ptr = nullptr;
    isize _stride_bytes = 0;
    isize _remaining_count = 0;
};

/// Non-owning view over elements of type T with a constant stride between elements
/// Useful for accessing interleaved data or subsets with regular spacing
///
/// The stride represents the byte offset between consecutive elements and can be:
/// - positive: forward iteration through memory
/// - negative: backward iteration through memory
/// - zero: all elements alias the same memory location (repeated element view)
///
/// Unlike span, strided_span is not necessarily contiguous, so it provides
/// start_ptr() instead of data() and has an is_contiguous() query.
///
/// IMPORTANT: Normal C++ alignment rules apply. The stride must ensure that all accessed
/// elements are properly aligned for type T. Using a stride less than alignof(T) or
/// strides that result in misaligned accesses can lead to undefined behavior.
///
/// DESIGN: Uses counting iterators with sentinel-based end detection. The begin() iterator
/// contains the full size and counts down, while end() returns cc::sentinel. This design
/// properly supports zero-stride iteration and eliminates any possibility of infinite loops.
template <class T>
struct cc::strided_span
{
    // types
public:
    using byte_ptr = std::conditional_t<std::is_const_v<T>, cc::byte const*, cc::byte*>;
    using iterator = cc::strided_iterator<T>;
    using sentinel = cc::sentinel;

private:
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast)
    [[nodiscard]] CC_FORCE_INLINE static constexpr byte_ptr to_byte_ptr(T* ptr)
    {
        return reinterpret_cast<byte_ptr>(ptr);
    }
    [[nodiscard]] CC_FORCE_INLINE static constexpr T* from_byte_ptr(byte_ptr ptr) { return reinterpret_cast<T*>(ptr); }
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)

    // construction
public:
    /// Default strided_span is empty: start_ptr() == nullptr, size() == 0, stride_bytes() == 0.
    constexpr strided_span() = default;

    // keep triviality
    constexpr strided_span(strided_span const&) = default;
    constexpr strided_span(strided_span&&) = default;
    constexpr strided_span& operator=(strided_span const&) = default;
    constexpr strided_span& operator=(strided_span&&) = default;
    constexpr ~strided_span() = default;

    /// Creates a strided_span viewing elements starting at ptr with the given size and stride_bytes.
    /// Precondition: size >= 0.
    constexpr explicit strided_span(T* ptr, isize size, isize stride_bytes) // NOLINT(bugprone-easily-swappable-parameters)
      : _start(strided_span::to_byte_ptr(ptr)), _size(size), _stride_bytes(stride_bytes)
    {
        CC_ASSERT(size >= 0, "strided_span size must be non-negative");
    }

    /// Creates a strided_span from an initializer_list.
    /// Only available when T is const; allows calling foo({1, 2, 3}) for foo(strided_span<int const>).
    /// The stride is set to sizeof(T) for contiguous iteration.
    /// WARNING: initializer_list temporaries are destroyed at the end of the full expression.
    /// Safe ONLY as an immediate function argument: foo({1, 2, 3}).
    /// NEVER assign to a variable: auto s = strided_span<int const>{1, 2, 3}; // DANGLING!
    constexpr strided_span(std::initializer_list<std::remove_const_t<T>> init)
        requires std::is_const_v<T>
      : _start(strided_span::to_byte_ptr(init.begin())),
        _size(static_cast<isize>(init.size())),
        _stride_bytes(static_cast<isize>(sizeof(T)))
    {
    }

    /// Creates a strided_span viewing the entire C array.
    /// Deduces size N from array type; implicit conversion allowed.
    /// The stride is set to sizeof(T) for contiguous iteration.
    template <std::size_t N>
    constexpr strided_span(T (&arr)[N])         //
      : _start(strided_span::to_byte_ptr(arr)), //
        _size(static_cast<isize>(N)),
        _stride_bytes(static_cast<isize>(sizeof(T)))
    {
    }

    /// Creates a strided_span from any contiguous container providing .data() and .size().
    /// The stride is set to sizeof(T) for contiguous iteration.
    /// The span does not own the container; the container must outlive the span.
    /// Passing a temporary container is safe when the span is used immediately (e.g., function argument).
    template <class Container>
        requires requires(Container&& c) {
            { c.data() } -> std::convertible_to<T*>;
            { c.size() } -> std::convertible_to<isize>;
        }
    constexpr strided_span(Container&& c) //
      : _start(strided_span::to_byte_ptr(c.data())),
        _size(static_cast<isize>(c.size())),
        _stride_bytes(static_cast<isize>(sizeof(T)))
    {
    }

    // element access
public:
    /// Returns a reference to the element at index i.
    /// Precondition: 0 <= i < size().
    [[nodiscard]] constexpr T& operator[](isize i) const
    {
        CC_ASSERT(0 <= i && i < _size, "index out of bounds");
        return *strided_span::from_byte_ptr(_start + i * _stride_bytes);
    }

    /// Returns a reference to the first element.
    /// Precondition: !empty().
    [[nodiscard]] constexpr T& front() const
    {
        CC_ASSERT(_size > 0, "front() called on empty strided_span");
        return *strided_span::from_byte_ptr(_start);
    }

    /// Returns a reference to the last element.
    /// Precondition: !empty().
    [[nodiscard]] constexpr T& back() const
    {
        CC_ASSERT(_size > 0, "back() called on empty strided_span");
        return *strided_span::from_byte_ptr(_start + (_size - 1) * _stride_bytes);
    }

    /// Returns a pointer to the first element.
    /// May be nullptr if the span is default-constructed or empty.
    /// Note: Unlike span::data(), this does not guarantee contiguous storage.
    [[nodiscard]] constexpr T* start_ptr() const { return strided_span::from_byte_ptr(_start); }

    // iterators
public:
    /// Returns an iterator to the first element.
    /// The iterator contains the size and counts down to zero.
    /// Enables range-based for loops.
    [[nodiscard]] constexpr iterator begin() const { return iterator(_start, _stride_bytes, _size); }

    /// Returns a sentinel representing the end of the range.
    /// The iterator compares against this sentinel by checking if remaining_count == 0.
    [[nodiscard]] constexpr sentinel end() const { return {}; }

    // queries
public:
    /// Returns the number of elements in the strided_span.
    [[nodiscard]] constexpr isize size() const { return _size; }
    /// Returns true if size() == 0.
    [[nodiscard]] constexpr bool empty() const { return _size == 0; }
    /// Returns the byte stride between consecutive elements.
    [[nodiscard]] constexpr isize stride_bytes() const { return _stride_bytes; }
    /// Returns true if the elements are contiguous in memory (stride_bytes == sizeof(T)).
    /// Size 0 or 1 spans are always considered contiguous.
    [[nodiscard]] constexpr bool is_contiguous() const
    {
        return _size <= 1 || static_cast<size_t>(_stride_bytes) == sizeof(T);
    }

    // conversions
public:
    /// Attempts to convert this strided_span to a contiguous span.
    /// Returns nullopt if the stride is not equal to sizeof(T).
    [[nodiscard]] constexpr cc::optional<cc::span<T>> try_to_span() const
    {
        if (is_contiguous())
            return cc::span<T>(start_ptr(), _size);
        return cc::nullopt;
    }

    // operations
public:
    /// Returns a new strided_span with reversed iteration order.
    /// The new span starts at the last element and has negated stride_bytes.
    /// For an empty span, returns an empty span.
    [[nodiscard]] constexpr strided_span reversed() const
    {
        auto const new_start = _start + (_size - 1) * _stride_bytes;
        return strided_span(strided_span::from_byte_ptr(new_start), _size, -_stride_bytes);
    }

    // factory methods
public:
    /// Creates a strided_span viewing a single element.
    /// The size is 1 and stride is sizeof(T) (normal element access).
    [[nodiscard]] static constexpr strided_span create_from_single(T& element)
    {
        return strided_span(&element, 1, static_cast<isize>(sizeof(T)));
    }

    /// Creates a strided_span that repeats the same element count times.
    /// The stride is 0, so all indices access the same memory location.
    /// Precondition: count >= 0.
    [[nodiscard]] static constexpr strided_span create_from_repeated(T& element, isize count)
    {
        CC_ASSERT(count >= 0, "count must be non-negative");
        return strided_span(&element, count, 0);
    }

    // members
private:
    byte_ptr _start = nullptr;
    isize _size = 0;
    isize _stride_bytes = 0;
};
