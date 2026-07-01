#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

namespace sg
{
/// The mutable entry point to a graphics backend and the factory for command lists and GPU
/// resources. This is an abstract interface: a backend library subclasses it directly (e.g.
/// sg::backend::vulkan::vulkan_context) and you obtain one via that backend's
/// sg::create_<backend>_context(config) factory. sg itself neither depends on nor knows the
/// concrete backend types — a "backend" could equally be a debug, cpu, or remote implementation.
class context
{
public:
    virtual ~context();

    /// Which kind of backend this context drives — a coarse tag, not the concrete type.
    [[nodiscard]] backend_kind backend() const { return _backend; }

    /// Opens a command list for recording.
    [[nodiscard]] virtual command_list_handle create_command_list() = 0;

    /// Allocates a GPU-resident buffer of the given size and usage.
    [[nodiscard]] virtual buffer_handle create_buffer(isize size_in_bytes, buffer_usage usage) = 0;

protected:
    explicit context(backend_kind backend);

    backend_kind _backend;
};
} // namespace sg
