#pragma once

#include <clean-core/common/assert.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/context.hh>

#include <memory>

namespace sg::backend::dx12
{
/// Per-backend creation config for the DirectX 12 context. Each flag is independent: enabling the
/// debug layer does not force WARP, and WARP does not imply the debug layer. This is why context
/// creation lives in the backend rather than in sg — each backend takes its own config type.
struct dx12_config
{
    /// Enable the D3D12 debug layer (validation) before device creation. Best-effort: if the layer
    /// isn't installed (Graphics Tools feature absent), creation proceeds without it.
    bool enable_debug_layer = false;

    /// Force the WARP software adapter instead of a hardware GPU. WARP runs headless, so it is the
    /// CI-friendly path; hardware is the default.
    bool use_warp = false;
};

/// DirectX 12 implementation of sg::context. Backends derive directly from the sg interfaces —
/// there is no separate bridge layer — and have full access to the protected state. Smurf-named
/// and namespaced (sg::backend::dx12) on purpose; see the sg coding guidelines. Members are public
/// and low-encapsulation.
///
/// The virtual sg::context methods are thin forwarders to backend-typed (create_dx12_*) methods:
/// downstream code that stays on dx12 works with dx12_* types directly, and the backend body never
/// re-casts back down from the abstract base. The heavier bodies live in dx12.cc; the ctor and the
/// one-line forwarders stay inline (they don't earn a .cc entry).
class dx12_context final : public sg::context
{
public:
    dx12_context(ComPtr<IDXGIFactory4> factory, ComPtr<ID3D12Device> device, ComPtr<ID3D12CommandQueue> queue)
      : sg::context(sg::backend_kind::dx12),
        _factory(cc::move(factory)),
        _device(cc::move(device)),
        _queue(cc::move(queue))
    {
    }

    ~dx12_context() override { shutdown(); } // ensures teardown before the base dtor's shut-down assert

    // backend-typed API — prefer these when you already hold a dx12_context

    [[nodiscard]] cc::result<std::unique_ptr<dx12_command_list>> create_dx12_command_list();
    [[nodiscard]] cc::result<dx12_buffer_handle> create_dx12_buffer(cc::isize size_in_bytes, sg::buffer_usage usage);
    void submit_dx12_command_list(std::unique_ptr<dx12_command_list> cmd);
    void drop_dx12_command_list(std::unique_ptr<dx12_command_list> cmd);

    // sg::context overrides — thin forwarders to the backend-typed methods above. The create ones use
    // result's explicit converting constructor (dx12_buffer_handle -> buffer_handle, and the unique_ptr
    // up-cast). The consuming ones re-wrap the moved-out pointer as the backend type — backends never
    // mix, so the static_cast is sound.

    [[nodiscard]] cc::result<std::unique_ptr<sg::command_list>> create_command_list() override
    {
        return cc::result<std::unique_ptr<sg::command_list>>(create_dx12_command_list());
    }

    [[nodiscard]] cc::result<sg::buffer_handle> create_buffer(cc::isize size_in_bytes, sg::buffer_usage usage) override
    {
        return cc::result<sg::buffer_handle>(create_dx12_buffer(size_in_bytes, usage));
    }

    void submit_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        submit_dx12_command_list(std::unique_ptr<dx12_command_list>(static_cast<dx12_command_list*>(cmd.release())));
    }

    void drop_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        drop_dx12_command_list(std::unique_ptr<dx12_command_list>(static_cast<dx12_command_list*>(cmd.release())));
    }

    void shutdown() override;

    ComPtr<IDXGIFactory4> _factory;
    ComPtr<ID3D12Device> _device;
    ComPtr<ID3D12CommandQueue> _queue;
};
} // namespace sg::backend::dx12

namespace sg
{
/// Creates a context on the DirectX 12 backend. Backend factories deliberately live in the `sg`
/// namespace (not the backend's) so they share a discoverable `sg::create_*_context` prefix while
/// taking a backend-specific config. sg itself neither depends on nor knows this backend; only a
/// caller that links the dx12 backend library sees this factory.
///
/// Returns an error (never asserts) on environment failure — no adapter, device creation refused,
/// etc. — so callers can fall back or surface it. The device-creation body lives in dx12.cc.
[[nodiscard]] cc::result<context_handle> create_dx12_context(backend::dx12::dx12_config const& config = {});
} // namespace sg
