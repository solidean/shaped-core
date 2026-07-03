#pragma once

#include <clean-core/common/assert.hh>
#include <clean-core/thread/mutex.hh>
#include <shaped-graphics/allocation_info.hh>
#include <shaped-graphics/backends/dx12/dx12_buffer.hh>
#include <shaped-graphics/backends/dx12/dx12_command_list.hh>
#include <shaped-graphics/backends/dx12/dx12_common.hh>
#include <shaped-graphics/backends/dx12/dx12_epoch.hh>
#include <shaped-graphics/backends/dx12/fwd.hh>
#include <shaped-graphics/context.hh>

#include <atomic>
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
    dx12_context(ComPtr<IDXGIFactory4> factory,
                 ComPtr<ID3D12Device> device,
                 ComPtr<ID3D12CommandQueue> queue,
                 ComPtr<ID3D12Fence> epoch_fence,
                 ComPtr<ID3D12Fence> submission_fence,
                 HANDLE fence_event)
      : sg::context(sg::backend_kind::dx12, sg::thread_model::multi_threaded),
        _factory(cc::move(factory)),
        _device(cc::move(device)),
        _queue(cc::move(queue)),
        _epoch_fence(cc::move(epoch_fence)),
        _submission_fence(cc::move(submission_fence)),
        _fence_event(fence_event)
    {
    }

    ~dx12_context() override { shutdown(); } // runs shutdown() before the base dtor asserts it

    // backend-typed API — prefer these when you already hold a dx12_context

    [[nodiscard]] cc::result<std::unique_ptr<dx12_command_list>> create_dx12_command_list();
    [[nodiscard]] cc::result<dx12_buffer_handle> create_dx12_buffer(cc::isize size_in_bytes,
                                                                    sg::buffer_usage usage,
                                                                    sg::allocation_info const& alloc);
    sg::submission_token submit_dx12_command_list(std::unique_ptr<dx12_command_list> cmd);
    void drop_dx12_command_list(std::unique_ptr<dx12_command_list> cmd);

    /// Stages a refcount-zero GPU resource for deferred deletion, attributed to the epoch it dies in
    /// (freed once that epoch retires). Called from ~dx12_buffer; safe to call from any thread.
    void schedule_deferred_deletion(dx12_expiring_resource expiring);

    // sg::context overrides — forward to the backend-typed methods above. A command list is only
    // ever a dx12 one here (backends never mix), so the downcast is sound; a CC_ASSERT'd dynamic_cast
    // guards against a foreign command list slipping in (compiled out in release).

    [[nodiscard]] cc::result<std::unique_ptr<sg::command_list>> create_command_list() override
    {
        return cc::result<std::unique_ptr<sg::command_list>>(create_dx12_command_list());
    }

    [[nodiscard]] cc::result<sg::buffer_handle> create_buffer(cc::isize size_in_bytes,
                                                              sg::buffer_usage usage,
                                                              sg::allocation_info const& alloc) override
    {
        return cc::result<sg::buffer_handle>(create_dx12_buffer(size_in_bytes, usage, alloc));
    }

    sg::submission_token submit_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        CC_ASSERT(dynamic_cast<dx12_command_list*>(cmd.get()) != nullptr, "command list is not a dx12 command list");
        return submit_dx12_command_list(
            std::unique_ptr<dx12_command_list>(static_cast<dx12_command_list*>(cmd.release())));
    }

    void drop_command_list(std::unique_ptr<sg::command_list> cmd) override
    {
        CC_ASSERT(dynamic_cast<dx12_command_list*>(cmd.get()) != nullptr, "command list is not a dx12 command list");
        drop_dx12_command_list(std::unique_ptr<dx12_command_list>(static_cast<dx12_command_list*>(cmd.release())));
    }

    // Epoch contract — bodies in dx12_epoch.cc. These return sg vocabulary types (no backend-typed
    // variant needed), so the whole body lives in the override.

    [[nodiscard]] sg::epoch current_epoch() const override { return _current_epoch; }
    [[nodiscard]] sg::epoch completed_epoch() const override;
    void advance_epoch(cc::optional<int> allowed_in_flight) override;
    void advance_epoch_and_wait_for_idle() override { advance_epoch(0); }
    void process_completed_epochs() override;
    void wait_for_epoch(sg::epoch e) override;
    void wait_for_next_inflight_epoch() override;
    [[nodiscard]] bool is_submission_complete(sg::submission_token token) const override;

    void shutdown() override;

    ComPtr<IDXGIFactory4> _factory;
    ComPtr<ID3D12Device> _device;
    ComPtr<ID3D12CommandQueue> _queue;

    // Epoch machinery. The epoch fence is signaled with the epoch value at the end of each epoch;
    // the submission fence is a per-command-list timeline on the same queue.
    ComPtr<ID3D12Fence> _epoch_fence;
    ComPtr<ID3D12Fence> _submission_fence;
    HANDLE _fence_event = nullptr; // reusable event for SetEventOnCompletion waits (driver thread only)

    // Written only by advance (externally synchronized), read concurrently by create/submit/drop.
    sg::epoch _current_epoch = sg::epoch::first;

    // create / submit / drop are thread-safe, so the shared bookkeeping they touch is synchronized:
    //  - _open_command_lists: bumped per create, dropped per submit/drop — a lock-free counter.
    //  - _next_submission: the next completion token; guarded together with the ExecuteCommandLists +
    //    Signal in submit so token order == queue/signal order (out-of-order signals could move the
    //    fence value backwards).
    //  - _allocators: the command-allocator pool (see dx12_allocator_pool).
    std::atomic<int> _open_command_lists = 0; // must reach 0 before advance — lists cannot span epochs
    cc::mutex<sg::submission_token> _next_submission{sg::submission_token::first};
    cc::mutex<dx12_allocator_pool> _allocators;

    cc::mutex<dx12_epoch_state> _epoch_state;
};
} // namespace sg::backend::dx12

namespace sg
{
/// Creates a dx12-backed sg::context. Returns an error (never asserts) on environment failure —
/// no adapter, device creation refused, etc. Only callers that link the dx12 backend see it.
[[nodiscard]] cc::result<context_handle> create_dx12_context(backend::dx12::dx12_config const& config = {});
} // namespace sg
