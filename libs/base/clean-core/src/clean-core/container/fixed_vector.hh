#pragma once

#include <clean-core/fwd.hh>

// TODO:
// - implement fixed_vector body
// - equality, order, hashing


/// Fixed-capacity vector of up to N elements of type T.
/// Similar to a vector but with compile-time maximum capacity.
/// Does not perform dynamic allocation - all storage is inline.
/// Supports runtime variable size up to the fixed capacity N.
template <class T, cc::isize N>
struct cc::fixed_vector
{
    // TODO: implement fixed_vector
    // Should have:
    // - inline storage for N elements
    // - runtime size tracking
    // - push_back/pop_back operations (without allocation)
    // - element access (operator[], front, back, data)
    // - iterators (begin, end)
    // - queries (size, empty, capacity)
    // - modifiers (clear, emplace_back, etc.)
    // - similar interface to cc::vector but with fixed capacity
};
