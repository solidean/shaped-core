#pragma once

#include <clean-core/common/assert.hh>
#include <shaped-graphics/context.hh>

namespace sg::backend::dx12
{
/// Per-backend creation config for the DirectX 12 context (adapter selection, debug layer, ...).
/// Empty for now; grows as the backend is implemented. This is why context creation lives in the
/// backend rather than in sg — each backend takes its own config type.
struct dx12_config
{
};

/// DirectX 12 implementation of sg::context. Backends derive directly from the sg interfaces —
/// there is no separate bridge layer — and have full access to the protected state. Smurf-named
/// and namespaced (sg::backend::dx12) on purpose; see the sg coding guidelines. Members are
/// defined inline: backends favor readable, low-encapsulation code.
class dx12_context final : public sg::context
{
public:
    dx12_context() : sg::context(sg::backend_kind::dx12) {}

    [[nodiscard]] sg::command_list_handle create_command_list() override { CC_UNREACHABLE("not implemented yet"); }

    [[nodiscard]] sg::buffer_handle create_buffer(cc::isize size_in_bytes, sg::buffer_usage usage) override
    {
        CC_UNREACHABLE("not implemented yet");
    }
};
} // namespace sg::backend::dx12

namespace sg
{
/// Creates a context on the DirectX 12 backend. Backend factories deliberately live in the `sg`
/// namespace (not the backend's) so they share a discoverable `sg::create_*_context` prefix while
/// taking a backend-specific config. sg itself neither depends on nor knows this backend; only a
/// caller that links the dx12 backend library sees this factory.
[[nodiscard]] inline context_handle create_dx12_context(backend::dx12::dx12_config const& = {})
{
    CC_UNREACHABLE("not implemented yet");
}
} // namespace sg
