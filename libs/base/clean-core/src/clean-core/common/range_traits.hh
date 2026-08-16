#pragma once

#include <clean-core/fwd.hh>

#include <concepts>

// Concepts describing what a range offers, kept deliberately light.
// Anything needing callable-signature reflection wants common/traits.hh, which is a much heavier include.

namespace cc
{
/// A range that knows its size and can be subscripted by an index — what sorting, searching and shuffling take.
/// Subscripting must accept an isize, and size() must convert to one.
/// Says nothing about contiguity or iterators: a strided_span models this, and so does a hand-written proxy.
template <class R>
concept indexed_range = requires(R& r, isize i) {
    { r.size() } -> std::convertible_to<isize>;
    r[i];
};
} // namespace cc
