#pragma once

#include <shaped-graphics/backend/fwd.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

#include <memory>

namespace sg
{
/// The mutable entry point to a graphics backend. Owns the backend bridge and is the factory
/// for command lists and GPU resources. You do not construct a context for a specific API
/// directly: each backend library provides an `sg::create_<backend>_context(config)` factory
/// (e.g. sg::create_dx12_context, sg::create_vulkan_context) that builds the backend bridge and
/// returns a context_handle. sg itself neither depends on nor knows the concrete backend types —
/// a "backend" could equally be a debug, cpu, or remote implementation.
class context
{
public:
    explicit context(std::shared_ptr<backend_context> backend);

    /// Which backend this context drives.
    [[nodiscard]] backend_kind backend() const;

    /// Opens a command list for recording.
    [[nodiscard]] command_list_handle create_command_list();

    /// Allocates a GPU-resident buffer of the given size and usage.
    [[nodiscard]] buffer_handle create_buffer(isize size_in_bytes, buffer_usage usage);

private:
    std::shared_ptr<backend_context> _backend;
};
} // namespace sg
