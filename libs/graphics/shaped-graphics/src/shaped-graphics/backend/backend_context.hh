#pragma once

#include <shaped-graphics/fwd.hh>
#include <shaped-graphics/types.hh>

namespace sg
{
/// Abstract per-backend context. `sg::context` performs sg-generic validation and then
/// delegates resource creation and submission to this interface, so the validation layer is
/// written once and every backend inherits it. Each backend static library provides exactly
/// one concrete, smurf-named implementation (e.g. sg::backend::dx12::dx12_context).
///
/// Non-copyable: a backend context owns device-level GPU state. Held via std::shared_ptr by
/// the owning sg::context.
class backend_context
{
public:
    backend_context() = default;
    backend_context(backend_context const&) = delete;
    backend_context& operator=(backend_context const&) = delete;
    virtual ~backend_context() = default;

    /// Which backend this implementation drives.
    [[nodiscard]] virtual backend_kind kind() const = 0;

    // Resource/command-list creation (open_command_list, create_buffer, ...) lands here as the
    // API grows; the first target is command-list buffer upload/download/copy.
};
} // namespace sg
