#pragma once

#include <shaped-graphics/fwd.hh>

/// An index buffer bound for an indexed draw: the buffer, the element width, and the byte sub-range to read.
/// Built directly, or via `raw_buffer::as_index_buffer(format)`.
/// A value type, which keeps the buffer alive via the held handle.

namespace sg
{
/// Bytes one index of `format` occupies.
[[nodiscard]] constexpr isize index_size_in_bytes(index_format format)
{
    return format == index_format::uint32 ? 4 : 2;
}

/// The byte alignment every index fetch must start at — the view's own offset, and the first index of each draw.
///
/// **A portable floor, hardcoded rather than queried**, the same approach as `constants_buffer_offset_alignment`: it is
/// what Metal requires, and neither D3D12 nor Vulkan asks for less than a caller obeying it already gives.
/// Enforced in the portable layer so a violation fails on whichever backend the author develops against, rather than
/// on the one that happens to mind.
///
/// **It bites only 16-bit indices, and it is the reason `draw_indexed` is checked and not just the bind.**
/// `index_range.offset` is a count of indices, so an odd first index into a `uint16` buffer starts the fetch 2 bytes
/// past a 4-byte boundary even when the view itself is perfectly aligned.
/// Metal then draws part of the mesh and reports nothing at all — not an error, not a validation message — which is
/// why the rule is stated here rather than left to a backend to discover.
///
/// A sub-mesh whose first index is odd is the realistic way to hit it.
/// The fixes are an even first index — pad the sub-mesh's index range — or 32-bit indices, where every index is
/// already 4-aligned and the rule cannot bind.
constexpr isize index_buffer_offset_alignment = 4;

/// Whether an indexed draw's fetch starts where `index_buffer_offset_alignment` requires.
///
/// `view_offset_in_bytes` is the bound `index_buffer_view`'s own offset and `first_index` is the draw's
/// `index_range.offset`, because the rule binds on what the two of them come to rather than on either alone.
///
/// Public so a caller can *ask* instead of tripping the assert — a mesh importer splitting sub-meshes is the case it
/// exists for, and rounding a sub-mesh's first index down to an even one is cheaper than discovering the rule from a
/// backend.
[[nodiscard]] constexpr bool is_aligned_index_fetch(index_format format, isize view_offset_in_bytes, isize first_index)
{
    return (view_offset_in_bytes + first_index * index_size_in_bytes(format)) % index_buffer_offset_alignment == 0;
}
} // namespace sg

struct sg::index_buffer_view
{
    raw_buffer_handle buffer;
    index_format format = index_format::uint16;

    /// First byte read from the buffer; must be a multiple of `index_buffer_offset_alignment`.
    isize offset_in_bytes = 0;

    isize size_in_bytes = -1; ///< bytes covered; -1 => to the end of the buffer
};
