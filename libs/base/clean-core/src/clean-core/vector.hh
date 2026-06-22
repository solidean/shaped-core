#pragma once

#include <clean-core/impl/allocating_container.hh>


// TODO:
// - sequence entry points
// - retyping APIs
// - equality, order, hashing
// - insert/emplace at arbitrary positions
// - push_back_range
// - contains/contains_where/find/find_where/count/count_where
// - sort
// - assign (replace parts of content)


/// Dynamically allocated vector of T elements with value semantics.
/// Similar to std::vector with support for growth operations (push, pop, resize).
/// Owns the underlying memory through cc::allocation<T>.
/// Compatible with the allocation-share protocol for efficient memory sharing.
/// Supports move semantics and allocator-aware construction.
template <class T>
struct cc::vector : private cc::allocating_container<T, vector<T>>
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>,
                  "allocations need to refer to non-const objects, not references/functions/void");

    using base = cc::allocating_container<T, vector<T>>;

    // element access
public:
    using base::operator[]; // access element by index
    using base::back;       // access last element
    using base::data;       // get pointer to underlying storage
    using base::front;      // access first element

    // iterators
public:
    using base::begin; // get pointer to first element
    using base::end;   // get pointer to one past last element

    // queries
public:
    using base::empty;      // check if vector is empty
    using base::size;       // get number of elements
    using base::size_bytes; // get total size in bytes

    // capacity queries
public:
    using base::capacity_back;         // get available capacity at back
    using base::has_capacity_back_for; // check if capacity exists for N elements at back

    /// Returns the total capacity (elements that can be stored without reallocation).
    /// For vector, capacity refers to back capacity (append capacity).
    [[nodiscard]] constexpr isize capacity() const { return size() + capacity_back(); }

    // factories
public:
    using base::create_copy_of;         // create deep copy from span
    using base::create_defaulted;       // create with default-constructed elements
    using base::create_filled;          // create with copies of a value
    using base::create_from_allocation; // create from existing allocation
    using base::create_uninitialized;   // create with uninitialized memory
    using base::create_with_capacity;   // create with reserved capacity
    using base::create_with_resource;   // create empty with specified memory resource

    // appending operations
public:
    using base::emplace_back;        // construct element at back (with allocation if needed)
    using base::emplace_back_stable; // construct element at back (requires capacity)
    using base::push_back;           // add element at back (with allocation if needed)
    using base::push_back_stable;    // add element at back (requires capacity)

    // single element removal
public:
    using base::pop_back;    // remove and return last element
    using base::remove_back; // remove last element (fast path, no return value)

    using base::pop_at;              // remove and return element at index (preserves order)
    using base::pop_at_unordered;    // remove and return element at index (O(1), does not preserve order)
    using base::remove_at;           // remove element at index (preserves order)
    using base::remove_at_unordered; // remove element at index (O(1), does not preserve order)

    // range removal
public:
    using base::remove_at_range;           // remove range [start, start+count) (preserves order)
    using base::remove_at_range_unordered; // remove range [start, start+count) (O(count), does not preserve order)
    using base::remove_from_to;            // remove range [start, end) (preserves order)
    using base::remove_from_to_unordered;  // remove range [start, end) (O(end-start), does not preserve order)

    // predicate-based removal
public:
    using base::remove_all_where;   // remove all elements matching predicate
    using base::remove_first_where; // remove first element matching predicate
    using base::remove_last_where;  // remove last element matching predicate

    using base::remove_all_value;   // remove all elements equal to value
    using base::remove_first_value; // remove first element equal to value
    using base::remove_last_value;  // remove last element equal to value

    using base::retain_all_where; // retain only elements matching predicate (remove others)

    // resizing operations
public:
    using base::resize_down_to;        // shrink to new_size by destroying trailing elements
    using base::resize_to_constructed; // resize with custom construction args
    using base::resize_to_defaulted;   // resize to new_size, default-construct new elements
    using base::resize_to_filled;      // resize to new_size, fill new elements with value
    using base::resize_to_uninitialized; // resize to new_size, new elements uninitialized, preserves existing (trivial types only)

    using base::clear_resize_to_constructed; // clear and resize with custom construction args
    using base::clear_resize_to_defaulted;   // clear and resize to new_size, default-construct all elements
    using base::clear_resize_to_filled;      // clear and resize to new_size, fill all elements with value
    using base::clear_resize_to_uninitialized; // clear and resize to new_size, all elements uninitialized (trivial types only)

    // capacity management
public:
    /// Ensures at least `count` elements can be stored without reallocation.
    /// Uses exponential growth strategy to amortize future reallocations.
    void reserve(isize count) { reserve_back(count - size()); }

    /// Ensures at least `count` elements can be stored without reallocation.
    /// Allocates exactly the needed space (rounded up to alignment).
    void reserve_exact(isize count) { reserve_back_exact(count - size()); }

    using base::reserve_back;       // ensure capacity for N more elements at back (exponential growth)
    using base::reserve_back_exact; // ensure capacity for N more elements at back (exact allocation)
    using base::shrink_to_fit;      // reduce capacity to match size

    // other mutations
public:
    using base::clear; // destroy all elements, size becomes 0
    using base::fill;  // fill all elements with value

    // TODO: insert(Iterator pos, T const& value) - insert element at position
    // TODO: insert(Iterator pos, T&& value) - insert element at position
    // TODO: insert(Iterator pos, isize count, T const& value) - insert multiple copies

    // ctors / allocation management
public:
    // vector has deep-copy value semantics
    using base::base; // inherit constructors (including initializer_list)
    vector() = default;
    ~vector() = default;
    vector(vector&&) = default;
    vector& operator=(vector&&) = default;
    vector(vector const&) = default;
    vector& operator=(vector const&) = default;

    using base::extract_allocation; // extract underlying allocation

    friend base;

private:
    static constexpr bool uses_capacity_front = false;
};
