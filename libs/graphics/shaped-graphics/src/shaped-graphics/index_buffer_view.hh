#pragma once

#include <shaped-graphics/fwd.hh>

/// An index buffer bound for an indexed draw: the buffer, the element width, and the byte sub-range to
/// read. Built directly or via `raw_buffer::as_index_buffer(format)`. A value type; keeps the buffer
/// alive via the held handle.

namespace sg
{
struct index_buffer_view
{
    raw_buffer_handle buffer;
    index_format format = index_format::uint16;
    isize offset_in_bytes = 0; ///< first byte read from the buffer
    isize size_in_bytes = -1;  ///< bytes covered; -1 => to the end of the buffer
};
} // namespace sg
