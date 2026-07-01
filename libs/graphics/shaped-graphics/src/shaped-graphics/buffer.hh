#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

namespace sg
{
/// A GPU-resident buffer. Its shape — size and usage — is immutable, so a buffer behaves like
/// a span over mutable GPU memory: it cannot be resized or repurposed, but its contents change
/// through command lists. There is no host-visible mapping; transfers go through sg's managed
/// PCIe path. Typically held via buffer_handle.
///
/// This is an abstract interface: a backend subclasses it directly (e.g.
/// sg::backend::vulkan::vulkan_buffer) and owns the actual GPU resource. The cheap shape metadata
/// lives here in the base as protected members with non-virtual accessors, so reading it costs no
/// virtual call and backends have full access to it — the coupling is intentional and fine.
class buffer
{
public:
    virtual ~buffer();

    /// Size of the buffer's GPU storage in bytes.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// How the buffer may be used (copy source/dest, vertex, index, ...).
    [[nodiscard]] buffer_usage usage() const { return _usage; }

protected:
    buffer(isize size_in_bytes, buffer_usage usage);

    // Shape metadata, shared by every backend. Protected, not private: backends set and read it
    // directly. sg is not defending these classes against their own subclasses.
    isize _size_in_bytes = 0;
    buffer_usage _usage = buffer_usage::none;
};
} // namespace sg
