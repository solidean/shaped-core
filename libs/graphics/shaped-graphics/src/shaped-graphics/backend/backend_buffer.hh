#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Abstract per-backend buffer: the GPU-resident storage an `sg::buffer` fronts. `sg::buffer`
/// keeps its cheap shape metadata (size, usage) inline and holds one of these for the actual
/// resource and its lifetime. Provided per backend as a smurf-named implementation
/// (e.g. sg::backend::vulkan::vulkan_buffer).
///
/// Non-copyable: it owns a GPU resource. Held via std::shared_ptr by the owning sg::buffer.
class backend_buffer
{
public:
    backend_buffer() = default;
    backend_buffer(backend_buffer const&) = delete;
    backend_buffer& operator=(backend_buffer const&) = delete;
    virtual ~backend_buffer() = default;
};
} // namespace sg
