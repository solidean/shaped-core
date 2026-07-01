#pragma once

#include <shaped-graphics/backend/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

#include <memory>

namespace sg
{
/// A GPU-resident buffer. Its shape — size and usage — is immutable, so a buffer behaves like
/// a span over mutable GPU memory: it cannot be resized or repurposed, but its contents change
/// through command lists. There is no host-visible mapping; transfers go through sg's managed
/// PCIe path. Shared and immutable in shape; typically held via buffer_handle.
///
/// The cheap shape metadata is kept inline (above the fold) for branch-free access; the actual
/// GPU resource lives behind the backend_buffer.
class buffer
{
public:
    buffer(isize size_in_bytes, buffer_usage usage, std::shared_ptr<backend_buffer> backend);

    /// Size of the buffer's GPU storage in bytes.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// How the buffer may be used (copy source/dest, vertex, index, ...).
    [[nodiscard]] buffer_usage usage() const { return _usage; }

private:
    // Shape metadata inline for cheap access; the backend owns the underlying GPU resource.
    isize _size_in_bytes = 0;
    buffer_usage _usage = buffer_usage::none;
    std::shared_ptr<backend_buffer> _backend;
};
} // namespace sg
