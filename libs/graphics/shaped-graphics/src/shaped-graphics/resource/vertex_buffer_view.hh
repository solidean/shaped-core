#pragma once

#include <shaped-graphics/fwd.hh>

/// A vertex buffer bound to an input slot for a draw: the buffer, the byte sub-range to read, and the
/// per-vertex stride. Built directly or via `raw_buffer::as_vertex_buffer<T>()`. A value type; keeps the
/// buffer alive via the held handle.

namespace sg
{
struct vertex_buffer_view
{
    raw_buffer_handle buffer;
    isize offset_in_bytes = 0; ///< first byte read from the buffer
    isize size_in_bytes = -1;  ///< bytes covered; -1 => to the end of the buffer
    isize stride_in_bytes = 0; ///< bytes between consecutive vertices (must match the pipeline's slot)
};
} // namespace sg
