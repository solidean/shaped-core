#pragma once

#include <clean-core/fwd.hh>
#include <clean-core/utility.hh>

namespace cc::impl
{
/// Calls destructors on [start, end) in reverse order.
/// Empty ranges (start == end) and nullptr are valid and result in a no-op.
/// Trivially destructible types are optimized out at compile time.
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

/// Default-constructs a count of objects using placement new.
/// dest_end is incremented for each successfully constructed object.
/// IMPORTANT: Assumes the objects at [*dest_end, *dest_end + count) are NOT yet constructed
/// (uninitialized memory). This function initializes the lifetime of objects starting at *dest_end. If default
/// construction throws, dest_end points to the element that threw (not yet constructed). If no exception occurs,
/// dest_end is updated to one past the last constructed object. count == 0 is valid and results in a no-op.
/// All objects are properly initialized via T(), which ensures zero-initialization for trivial types (e.g., int).
///
/// Usage pattern:
///   auto obj_start = (T*)uninitialized_memory;
///   auto obj_end = obj_start;
///   default_create_objects_to(obj_end, count);
///   // [obj_start, obj_end) is now the constructed live range
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

/// Fill-constructs a count of objects by copy-constructing from a single value using placement new.
/// dest_end is incremented for each successfully constructed object.
/// IMPORTANT: Assumes the objects at [*dest_end, *dest_end + count) are NOT yet constructed
/// (uninitialized memory). This function initializes the lifetime of objects starting at *dest_end. If copy
/// construction throws, dest_end points to the element that threw (not yet constructed). If no exception occurs,
/// dest_end is updated to one past the last constructed object. count == 0 is valid and results in a no-op.
///
/// Usage pattern:
///   auto obj_start = (T*)uninitialized_memory;
///   auto obj_end = obj_start;
///   fill_create_objects_to(obj_end, count, value);
///   // [obj_start, obj_end) is now the constructed live range, each element a copy of value
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

/// Copy-constructs objects from [src_start, src_end) using placement new.
/// dest_end is incremented for each successfully constructed object.
/// IMPORTANT: Assumes the objects at [*dest_end, *dest_end + (src_end - src_start)) are NOT yet constructed
/// (uninitialized memory). This function initializes the lifetime of objects starting at *dest_end. If copy
/// construction throws, dest_end points to the element that threw (not yet constructed). If no exception occurs,
/// dest_end is updated to one past the last constructed object. Empty ranges (src_start == src_end) and nullptr are
/// valid and result in a no-op. Trivially copyable types are optimized to use memcpy at compile time.
///
/// Usage pattern:
///   auto obj_start = (T*)uninitialized_memory;
///   auto obj_end = obj_start;
///   copy_create_objects_to(obj_end, src, src + count);
///   // [obj_start, obj_end) is now the constructed live range
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

/// Move-constructs objects from [src_start, src_end) using placement new.
/// dest_end is incremented for each successfully constructed object.
/// IMPORTANT: Assumes the objects at [*dest_end, *dest_end + (src_end - src_start)) are NOT yet constructed
/// (uninitialized memory). This function initializes the lifetime of objects starting at *dest_end. We keep the
/// T*& dest_end design for consistency with copy_create_objects_to, but do not promise exception safety if move
/// constructors throw. Empty ranges (src_start == src_end) and nullptr are valid and result in a no-op. Trivially
/// copyable types are optimized to use memcpy at compile time.
///
/// Usage pattern:
///   auto obj_start = (T*)uninitialized_memory;
///   auto obj_end = obj_start;
///   move_create_objects_to(obj_end, src, src + count);
///   // [obj_start, obj_end) is now the constructed live range
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

/// Move-constructs objects from [src_start, src_end) using placement new in reverse order.
/// dest_start is decremented for each successfully constructed object.
/// IMPORTANT: Assumes the objects at [*dest_start - (src_end - src_start), *dest_start) are NOT yet constructed
/// (uninitialized memory). This function initializes the lifetime of objects ending at *dest_start, moving backwards.
/// Constructs from src_end-1 down to src_start, placing them at dest_start-1, dest_start-2, etc.
/// Empty ranges (src_start == src_end) and nullptr are valid and result in a no-op. Trivially copyable types are
/// optimized to use memcpy at compile time.
///
/// Usage pattern:
///   auto obj_end = (T*)uninitialized_memory_end;
///   auto obj_start = obj_end;
///   move_create_objects_to_reverse(obj_start, src, src + count);
///   // [obj_start, obj_end) is now the constructed live range (constructed in reverse)
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

/// Compacts objects by moving [src_start, src_end) to [dest_start, ...) within the same allocation.
/// Designed for removal operations where dest_start < src_start (moving elements backward to close a gap).
/// PRECONDITIONS:
///   - dest_start < src_start (target is before source, eliminating possibility of forward overlap issues)
///   - Both ranges are within the same allocation
///   - All objects in [dest_start, dest_start + (src_end - src_start)) are alive (will be overwritten)
///   - All objects in [src_start, src_end) are alive (will be moved-from)
/// POSTCONDITIONS:
///   - Objects in [dest_start, dest_start + (src_end - src_start)) contain moved values
///   - Objects in [src_start, src_end) are in moved-from state (still alive, must be destroyed separately)
/// Uses forward iteration which is safe since dest_start < src_start.
/// Trivially copyable types are optimized to use memmove (which handles all overlaps correctly).
/// Empty ranges (src_start == src_end) are valid and result in a no-op.
///
/// Usage pattern (closing a gap after removal):
///   // Remove element at idx from [obj_start, obj_end)
///   compact_move_objects_backward(obj_start + idx, obj_start + idx + 1, obj_end);
///   // obj_start[idx] through obj_end[-2] now contain the compacted elements
///   // obj_end[-1] is in moved-from state, destroy it
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

/// Copy-assigns objects from [src_start, src_end) using copy assignment operator.
/// dest_end is incremented for each successfully assigned object.
/// IMPORTANT: Assumes the objects at [*dest_end, *dest_end + (src_end - src_start)) are already constructed (alive).
/// If copy assignment throws, dest_end points to the element that threw (partially modified state).
/// If no exception occurs, dest_end is updated to one past the last assigned object.
/// Empty ranges (src_start == src_end) and nullptr are valid and result in a no-op.
/// Trivially copyable types are optimized to use memcpy at compile time.
///
/// Usage pattern:
///   auto obj_end = obj_start;
///   copy_assign_objects_to(obj_end, src, src + count);
///   // [obj_start, obj_end) is now the validly assigned range
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

/// Fill-assigns a count of objects by copy-assigning from a single value using copy assignment operator.
/// dest_end is incremented for each successfully assigned object.
/// IMPORTANT: Assumes the objects at [*dest_end, *dest_end + count) are already constructed (alive).
/// If copy assignment throws, dest_end points to the element that threw (partially modified state).
/// If no exception occurs, dest_end is updated to one past the last assigned object.
/// count == 0 is valid and results in a no-op.
///
/// Usage pattern:
///   auto obj_end = obj_start;
///   fill_assign_objects_to(obj_end, count, value);
///   // [obj_start, obj_end) is now the validly assigned range, each element a copy of value
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
