#pragma once

#include <clean-core/common/assert.hh>
#include <shaped-graphics/context.hh>

namespace sg::backend::vulkan
{
/// Per-backend creation config for the Vulkan context (device selection, validation layers, ...).
/// Empty for now; grows as the backend is implemented. This is why context creation lives in the
/// backend rather than in sg — each backend takes its own config type.
struct vulkan_config
{
};

/// Vulkan implementation of sg::context. Backends derive directly from the sg interfaces — there
/// is no separate bridge layer — and have full access to the protected state. Smurf-named and
/// namespaced (sg::backend::vulkan) on purpose; see the sg coding guidelines. Members are defined
/// inline: backends favor readable, low-encapsulation code.
class vulkan_context final : public sg::context
{
public:
    vulkan_context() : sg::context(sg::backend_kind::vulkan) {}

    [[nodiscard]] sg::command_list_handle create_command_list() override { CC_UNREACHABLE("not implemented yet"); }

    [[nodiscard]] sg::buffer_handle create_buffer(cc::isize size_in_bytes, sg::buffer_usage usage) override
    {
        CC_UNREACHABLE("not implemented yet");
    }
};
} // namespace sg::backend::vulkan

namespace sg
{
/// Creates a context on the Vulkan backend. Backend factories deliberately live in the `sg`
/// namespace (not the backend's) so they share a discoverable `sg::create_*_context` prefix while
/// taking a backend-specific config. sg itself neither depends on nor knows this backend; only a
/// caller that links the vulkan backend library sees this factory.
[[nodiscard]] inline context_handle create_vulkan_context(backend::vulkan::vulkan_config const& = {})
{
    CC_UNREACHABLE("not implemented yet");
}
} // namespace sg
