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
/// Creation config for the dx12 context. The two flags are independent.
struct dx12_config
{
    /// Enable the D3D12 debug/validation layer. Best-effort — skipped if it isn't installed.
    bool enable_debug_layer = false;

    /// Use the WARP software adapter instead of a hardware GPU. Runs headless (good for CI).
    bool use_warp = false;
};

/// DirectX 12 implementation of sg::context. The sg::context virtuals are thin forwarders to the
/// backend-typed create_dx12_* methods — prefer those when you hold a dx12_context. Bodies in dx12.cc.
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

    ~dx12_context() override { shutdown(); } // runs shutdown() before the base dtor asserts it

    // backend-typed API — prefer these when you already hold a dx12_context

    [[nodiscard]] cc::result<std::unique_ptr<dx12_command_list>> create_dx12_command_list();
    [[nodiscard]] cc::result<dx12_buffer_handle> create_dx12_buffer(cc::isize size_in_bytes, sg::buffer_usage usage);
    void submit_dx12_command_list(std::unique_ptr<dx12_command_list> cmd);
    void drop_dx12_command_list(std::unique_ptr<dx12_command_list> cmd);

    // sg::context overrides — forward to the backend-typed methods above. The static_cast down is
    // sound: backends never mix.

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
/// Creates a dx12-backed sg::context. Returns an error (never asserts) on environment failure —
/// no adapter, device creation refused, etc. Only callers that link the dx12 backend see it.
[[nodiscard]] cc::result<context_handle> create_dx12_context(backend::dx12::dx12_config const& config = {});
} // namespace sg
