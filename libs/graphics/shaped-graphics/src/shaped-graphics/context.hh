#pragma once

#include <clean-core/error/result.hh>
#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

#include <memory>

namespace sg
{
/// Mutable entry point to a graphics backend: the factory for command lists and GPU resources.
/// Abstract — a backend subclasses it (e.g. sg::backend::vulkan::vulkan_context) and you obtain one
/// from that backend's sg::create_<backend>_context(config).
///
/// Must outlive every command list and resource it creates, and be shut down before destruction.
class context
{
public:
    virtual ~context();

    /// The backend kind driving this context — a coarse tag, not the concrete type.
    [[nodiscard]] backend_kind backend() const { return _backend; }

    /// Opens a new command list, already recording. Single-use: submit or drop it once.
    [[nodiscard]] virtual cc::result<std::unique_ptr<command_list>> create_command_list() = 0;

    /// Allocates a GPU-resident buffer. Size must be >= 0 (0 is a valid empty buffer).
    [[nodiscard]] virtual cc::result<buffer_handle> create_buffer(isize size_in_bytes, buffer_usage usage) = 0;

    /// Submits a command list for execution and consumes it.
    virtual void submit_command_list(std::unique_ptr<command_list> cmd) = 0;

    /// Discards a command list unsubmitted and consumes it — same as letting it go out of scope.
    virtual void drop_command_list(std::unique_ptr<command_list> cmd) = 0;

    /// Releases all backend resources; the context is unusable afterwards. Idempotent, and run
    /// automatically on destruction.
    virtual void shutdown();

    /// Whether shutdown() has run.
    [[nodiscard]] bool is_shut_down() const { return _is_shut_down; }

protected:
    explicit context(backend_kind backend);

    backend_kind _backend;
    bool _is_shut_down = false;
};
} // namespace sg
