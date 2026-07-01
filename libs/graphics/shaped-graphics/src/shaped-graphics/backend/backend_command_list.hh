#pragma once

#include <shaped-graphics/fwd.hh>

namespace sg
{
/// Abstract per-backend command list. Records GPU work (uploads, downloads, copies, ...) for
/// later submission. `sg::command_list` wraps one of these and adds sg-generic validation
/// before each backend call. Provided per backend as a smurf-named implementation
/// (e.g. sg::backend::vulkan::vulkan_command_list).
///
/// Non-copyable: recording state is not shareable. Held via std::shared_ptr by the owning
/// sg::command_list.
class backend_command_list
{
public:
    backend_command_list() = default;
    backend_command_list(backend_command_list const&) = delete;
    backend_command_list& operator=(backend_command_list const&) = delete;
    virtual ~backend_command_list() = default;

    // Recording API (copy_buffer, upload_buffer, download_buffer, ...) lands here as the API grows.
};
} // namespace sg
