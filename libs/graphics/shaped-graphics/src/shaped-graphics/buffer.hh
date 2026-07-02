#pragma once

#include <clean-core/common/utility.hh>
#include <clean-core/container/vector.hh>
#include <clean-core/function/unique_function.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

namespace sg
{
/// A GPU-resident buffer with immutable shape (size + usage) — like a span over mutable GPU memory:
/// contents change through command lists, but it can't be resized or repurposed. No host-visible
/// mapping (transfers go through command lists). Size 0 is a valid empty buffer. Held via buffer_handle.
///
/// Abstract: a backend subclasses it and owns the GPU resource. Shape metadata lives here as
/// protected members that backends read and set directly.
class buffer
{
public:
    virtual ~buffer();

    /// Size of the buffer's GPU storage in bytes.
    [[nodiscard]] isize size_in_bytes() const { return _size_in_bytes; }

    /// How the buffer may be used (copy source/dest, vertex, index, ...).
    [[nodiscard]] buffer_usage usage() const { return _usage; }

    /// Registers a callback to run once this buffer's GPU storage is released *and* no longer in
    /// flight (its owning epoch has retired). The feedback point for reclaiming externally-owned
    /// backing memory (e.g. placed resources on a custom allocator). Do not assume which thread runs it.
    void add_finalizer(cc::unique_function<void()> finalizer) { _finalizers.push_back(cc::move(finalizer)); }

protected:
    buffer(isize size_in_bytes, buffer_usage usage);

    isize _size_in_bytes = 0;
    buffer_usage _usage = buffer_usage::none;
    cc::vector<cc::unique_function<void()>> _finalizers;
};
} // namespace sg
