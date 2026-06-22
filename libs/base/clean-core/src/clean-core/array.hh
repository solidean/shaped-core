#pragma once

#include <clean-core/impl/allocating_container.hh>


// TODO:
// - sequence entry points
// - retyping APIs
// - resize APIs? -> would be totally fine I think
// - equality, order, hashing


/// Dynamically allocated array of T elements with value semantics.
/// Similar to std::vector but without growth operations (push, pop, resize).
/// Owns the underlying memory through cc::allocation<T>.
/// Compatible with the allocation-share protocol for efficient memory sharing.
/// Supports move semantics and allocator-aware construction.
template <class T>
struct cc::array : private cc::allocating_container<T, array<T>>
{
    static_assert(std::is_object_v<T> && !std::is_const_v<T>,
                  "allocations need to refer to non-const objects, not references/functions/void");

    using base = cc::allocating_container<T, array<T>>;

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
    using base::empty;      // check if array is empty
    using base::size;       // get number of elements
    using base::size_bytes; // get total size in bytes

    // factories
public:
    using base::create_copy_of;         // create deep copy from span
    using base::create_defaulted;       // create with default-constructed elements
    using base::create_filled;          // create with copies of a value
    using base::create_from_allocation; // create from existing allocation
    using base::create_uninitialized;   // create with uninitialized memory
    using base::create_with_resource;   // create empty with specified memory resource

    // content mutation
public:
    using base::fill; // fill all elements with value

    // allocation management
public:
    using base::extract_allocation; // extract underlying allocation

    // array has deep-copy value semantics
    using base::base; // inherit constructors (including initializer_list)
    array() = default;
    ~array() = default;
    array(array&&) = default;
    array& operator=(array&&) = default;
    array(array const&) = default;
    array& operator=(array const&) = default;

    friend base;

private:
    static constexpr bool uses_capacity_front = false;
};
