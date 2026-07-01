#pragma once

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backend/backend_context.hh>
#include <shaped-graphics/fwd.hh>

namespace sg::backend::dx12
{
/// Per-backend creation config for the DirectX 12 context (adapter selection, debug layer, ...).
/// Empty for now; grows as the backend is implemented. This is why context creation lives in the
/// backend rather than in sg — each backend takes its own config type.
struct dx12_config
{
};

/// DirectX 12 backend context. Smurf-named and namespaced (sg::backend::dx12) on purpose — see the
/// sg coding guidelines: backend types are kept greppable and non-colliding, code is duplicated
/// across backends rather than abstracted, and backends favor readable, low-encapsulation code
/// (small members defined inline in the header).
class dx12_context final : public sg::backend_context
{
public:
    [[nodiscard]] sg::backend_kind kind() const override { return sg::backend_kind::dx12; }
};
} // namespace sg::backend::dx12

namespace sg
{
/// Creates a context on the DirectX 12 backend. Backend factories deliberately live in the `sg`
/// namespace (not the backend's) so they share a discoverable `sg::create_*_context` prefix while
/// taking a backend-specific config. sg itself neither depends on nor knows this backend; only
/// the caller that links the dx12 backend library sees this factory.
[[nodiscard]] inline context_handle create_dx12_context(backend::dx12::dx12_config const& = {})
{
    CC_UNREACHABLE("not implemented yet");
}
} // namespace sg
