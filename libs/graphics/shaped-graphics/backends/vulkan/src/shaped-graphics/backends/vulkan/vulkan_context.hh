#pragma once

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backend/backend_context.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::vulkan
{
/// Per-backend creation config for the Vulkan context (device selection, validation layers, ...).
/// Empty for now; grows as the backend is implemented. This is why context creation lives in the
/// backend rather than in sg — each backend takes its own config type.
struct vulkan_config
{
};

/// Vulkan backend context. Smurf-named and namespaced (sg::backend::vulkan) on purpose — see the
/// sg coding guidelines: backend types are kept greppable and non-colliding, code is duplicated
/// across backends rather than abstracted, and backends favor readable, low-encapsulation code
/// (small members defined inline in the header).
class vulkan_context final : public sg::backend_context
{
public:
    [[nodiscard]] sg::backend_kind kind() const override { return sg::backend_kind::vulkan; }
};
} // namespace sg::backend::vulkan

namespace sg
{
/// Creates a context on the Vulkan backend. Backend factories deliberately live in the `sg`
/// namespace (not the backend's) so they share a discoverable `sg::create_*_context` prefix while
/// taking a backend-specific config. sg itself neither depends on nor knows this backend; only
/// the caller that links the vulkan backend library sees this factory.
[[nodiscard]] inline context_handle create_vulkan_context(backend::vulkan::vulkan_config const& = {})
{
    CC_UNREACHABLE("not implemented yet");
}
} // namespace sg
