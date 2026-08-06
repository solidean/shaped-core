#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/fwd.hh>

// Raw object-lifetime primitives behind the allocating containers.
// Every function here shares one contract, so the functions themselves document only what differs.
//
// A `_to` function takes its destination as a cursor by reference and advances it past each object it touches, which is how the caller ends up holding the live range:
//
//   auto obj_start = (T*)uninitialized_memory;
//   auto obj_end = obj_start;
//   copy_create_objects_to(obj_end, src, src + count);
//   // [obj_start, obj_end) is now the constructed live range
//
// A `*_create_*` function requires the destination to be UNINITIALIZED and begins the objects' lifetimes there.
// An `*_assign_*` function requires the destination objects to be ALIVE already.
// Empty ranges, a null pointer and count == 0 are all valid and do nothing.
// Where a function copies or moves a RANGE, a trivially copyable T takes a memcpy — memmove where the ranges may overlap — instead of a per-element loop.
// The default_ and fill_ functions always loop, since there is no source range to copy from.
// On a throw the cursor delimits the objects handled so far, so the caller can still destroy exactly them.
// The two move_ functions promise nothing beyond that, since a throwing move constructor has already lost the source.
//
namespace cc::impl
{
/// Calls destructors on [start, end) in reverse order.
/// A trivially destructible T compiles out entirely.
template <class T>
constexpr void destroy_objects_in_reverse(T* start, T* end)
{
    static_assert(sizeof(T) > 0, "T must be a complete type (did you forget to include a header?)");

    if constexpr (!std::is_trivially_destructible_v<T>)
    {
        while (end != start)
        {
            --end;
            end->~T();
        }
    }
}

/// Default-constructs `count` objects at the cursor.
/// Each is built with T(), so a trivial type such as int is zero-initialized rather than left indeterminate.
template <class T>
constexpr void default_create_objects_to(T*& dest_end, isize count)
{
    static_assert(sizeof(T) > 0, "T must be a complete type (did you forget to include a header?)");
    static_assert(std::is_default_constructible_v<T>, "T must be default constructible");

    // Always construct with T() to ensure proper initialization
    // (for trivial types like int, this ensures zero-initialization)
    for (isize i = 0; i < count; ++i)
    {
        new (cc::placement_new, dest_end) T();
        ++dest_end;
    }
}

/// Copy-constructs `count` objects at the cursor, each from `value`.
template <class T>
constexpr void fill_create_objects_to(T*& dest_end, isize count, T const& value)
{
    static_assert(sizeof(T) > 0, "T must be a complete type (did you forget to include a header?)");
    static_assert(std::is_copy_constructible_v<T>, "T must be copy constructible");

    for (isize i = 0; i < count; ++i)
    {
        new (cc::placement_new, dest_end) T(value);
        ++dest_end;
    }
}

/// Copy-constructs the objects of [src_start, src_end) at the cursor.
template <class T>
constexpr void copy_create_objects_to(T*& dest_end, T const* src_start, T const* src_end)
{
    static_assert(sizeof(T) > 0, "T must be a complete type (did you forget to include a header?)");
    static_assert(std::is_copy_constructible_v<T>, "T must be copy constructible");

    if constexpr (std::is_trivially_copyable_v<T>)
    {
        auto const size = src_end - src_start;
        if (size > 0)
        {
            cc::memcpy(dest_end, src_start, size * sizeof(T));
            dest_end += size;
        }
    }
    else
    {
        while (src_start != src_end)
        {
            new (cc::placement_new, dest_end) T(*src_start);
            ++dest_end;
            ++src_start;
        }
    }
}

/// Move-constructs the objects of [src_start, src_end) at the cursor.
/// The cursor is by-reference for consistency with copy_create_objects_to; unlike it, this promises nothing if a move constructor throws.
template <class T>
constexpr void move_create_objects_to(T*& dest_end, T* src_start, T* src_end)
{
    static_assert(sizeof(T) > 0, "T must be a complete type (did you forget to include a header?)");
    static_assert(std::is_move_constructible_v<T>, "T must be move constructible");

    if constexpr (std::is_trivially_copyable_v<T>)
    {
        auto const size = src_end - src_start;
        if (size > 0)
        {
            cc::memcpy(dest_end, src_start, size * sizeof(T));
            dest_end += size;
        }
    }
    else
    {
        while (src_start != src_end)
        {
            new (cc::placement_new, dest_end) T(cc::move(*src_start));
            ++dest_end;
            ++src_start;
        }
    }
}

/// Move-constructs the objects of [src_start, src_end) at the cursor, in reverse: from src_end - 1 down to src_start.
/// The cursor is the destination END and moves backwards, so it finishes at the start of the constructed range.
template <class T>
constexpr void move_create_objects_to_reverse(T*& dest_start, T* src_start, T* src_end)
{
    static_assert(sizeof(T) > 0, "T must be a complete type (did you forget to include a header?)");
    static_assert(std::is_move_constructible_v<T>, "T must be move constructible");

    if constexpr (std::is_trivially_copyable_v<T>)
    {
        auto const size = src_end - src_start;
        if (size > 0)
        {
            dest_start -= size;
            cc::memcpy(dest_start, src_start, size * sizeof(T));
        }
    }
    else
    {
        while (src_start != src_end)
        {
            --src_end;
            new (cc::placement_new, dest_start - 1) T(cc::move(*src_end));
            --dest_start; // _after_ construction so exceptions leave dest_start pointing to the constructed range
        }
    }
}

/// Compacts objects by moving [src_start, src_end) down to [dest_start, ...) within the same allocation, to close a gap left by a removal.
/// Forward iteration is what makes this safe, and it is only safe because dest_start does not exceed src_start.
///
/// PRECONDITIONS:
///   - dest_start <= src_start, so the ranges cannot overlap forwards; equal ranges are legal and become a no-op memmove
///   - both ranges lie in the same allocation
///   - the objects in [dest_start, dest_start + (src_end - src_start)) are alive, and will be overwritten
///   - the objects in [src_start, src_end) are alive, and will be moved from
///
/// The moved-from tail in [src_start, src_end) stays alive and must be destroyed separately:
///
///   compact_move_objects_backward(obj_start + idx, obj_start + idx + 1, obj_end);
///   --obj_end;
///   obj_end->~T();
template <class T>
constexpr void compact_move_objects_backward(T* dest_start, T* src_start, T* src_end)
{
    static_assert(sizeof(T) > 0, "T must be a complete type (did you forget to include a header?)");
    static_assert(std::is_move_assignable_v<T>, "T must be move assignable");

    CC_ASSERT(dest_start <= src_start, "compact_move_objects_backward requires dest_start < src_start");

    if constexpr (std::is_trivially_copyable_v<T>)
    {
        // memmove handles all overlap cases correctly (forward, backward, or identical ranges)
        auto const size = src_end - src_start;
        if (size > 0)
        {
            std::memmove(dest_start, src_start, size * sizeof(T));
        }
    }
    else
    {
        // Forward iteration is safe: we're moving backward (dest < src)
        // Each assignment completes before we read the next source element
        while (src_start != src_end)
        {
            *dest_start = cc::move(*src_start);
            ++dest_start;
            ++src_start;
        }
    }
}

/// Copy-assigns the objects of [src_start, src_end) over the live objects at the cursor.
/// A throw leaves the assigned-over elements in a partially modified state, not an invalid one.
template <class T>
constexpr void copy_assign_objects_to(T*& dest_end, T const* src_start, T const* src_end)
{
    static_assert(sizeof(T) > 0, "T must be a complete type (did you forget to include a header?)");
    static_assert(std::is_copy_assignable_v<T>, "T must be copy assignable");

    if constexpr (std::is_trivially_copyable_v<T>)
    {
        auto const size = src_end - src_start;
        if (size > 0)
        {
            cc::memcpy(dest_end, src_start, size * sizeof(T));
            dest_end += size;
        }
    }
    else
    {
        while (src_start != src_end)
        {
            *dest_end = *src_start;
            ++dest_end;
            ++src_start;
        }
    }
}

/// Copy-assigns `count` copies of `value` over the live objects at the cursor.
/// A throw leaves the assigned-over elements in a partially modified state, not an invalid one.
template <class T>
constexpr void fill_assign_objects_to(T*& dest_end, isize count, T const& value)
{
    static_assert(sizeof(T) > 0, "T must be a complete type (did you forget to include a header?)");
    static_assert(std::is_copy_assignable_v<T>, "T must be copy assignable");

    for (isize i = 0; i < count; ++i)
    {
        *dest_end = value;
        ++dest_end;
    }
}
} // namespace cc::impl
