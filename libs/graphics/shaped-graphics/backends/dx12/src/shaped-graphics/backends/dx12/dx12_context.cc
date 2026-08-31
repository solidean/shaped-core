// dx12_context: device-level lifetime bodies (shutdown / teardown). The heavier bring-up path
// lives in dx12_context.create.cc.

#include <clean-core/record/domain.hh>
#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
CC_REC_DEFINE_DOMAIN(g_rec_domain, "sg.dx12");

void dx12_context::shutdown()
{
    if (_is_shut_down)
        return;

    // Release per-context routine instances first: they may cache epoch/allocator-managed resources
    // (e.g. an init_once buffer) that must be freed before the resource systems below are torn down.
    routines.clear();

    // Advance-and-wait-for-idle drains the GPU, then closes and retires the final epoch — freeing
    // every resource (in-flight and staged) and running finalizers — before the device is released.
    // Externally synchronized: no create/submit/drop may run concurrently with shutdown.
    if (_queue && _epoch_fence)
        advance_epoch_and_wait_for_idle();

    // Drain + join the download actor and release the ring buffers while the submission fence is still
    // alive (the actor may block on it). The GPU is idle by now, so pending copies complete promptly.
    _download_inline.shutdown();
    _upload_inline.shutdown();
    // The async upload + download actors run on independent copy queues, which advance-and-wait did not drain, so their shutdown waits for those queues to idle.
    // Do it while the copy queues and fences are alive.
    _upload_async.shutdown();
    _download_async.shutdown();

    // Both the direct and copy queues are idle now.
    // The async actor's own shutdown may have dropped the last reference to a buffer — its in-flight upload — after the final advance already ran, staging a fresh deferred deletion.
    // Copy-deferred hold-backs may also still be waiting on the now fully signaled copy fence.
    // Nothing else will sweep these, so release them here while the device is alive.
    {
        cc::vector<dx12_expiring_resource> leftover = _epoch_state.lock(
            [](dx12_epoch_state& s)
            {
                cc::vector<dx12_expiring_resource> out = cc::move(s.staged);
                for (auto& r : s.copy_deferred)
                    out.push_back(cc::move(r));
                s.staged = {};
                s.copy_deferred = {};
                return out;
            });
        cc::vector<cc::unique_function<void()>> finalizers;
        for (auto& r : leftover)
            release_expiring(r, finalizers);
        for (auto& f : finalizers)
            f();
    }

    _cmd_pool.shutdown();
    _query_system.shutdown();

    // After the leftover sweep above, so any group a still-expiring resource held has already been released.
    // Groups outliving this simply destroy their fences instead of returning to a list that is gone.
    _group_pool.shutdown();

    _submission_fence.Reset();
    _epoch_fence.Reset();

    // Last thing before the device goes: after this no debug-layer message can reach a context that is on its way out.
    unregister_message_callback();

    // Release the device-level COM objects.
    _queue.Reset();
    _device.Reset();
    _factory.Reset();
    _is_shut_down = true;
}
} // namespace sg::backend::dx12

cc::result<sg::gpu_memory_usage> sg::backend::dx12::dx12_context::query_gpu_memory() const
{
    if (_adapter == nullptr)
        return cc::error("IDXGIAdapter3 unavailable, so this runtime cannot report a video memory budget");

    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    if (auto const hr = _adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info); FAILED(hr))
        return cc::error("QueryVideoMemoryInfo failed");

    // The LOCAL segment is memory on the card; NON_LOCAL is system memory the GPU may reach across the bus, and adding
    // them would report a budget far larger than anything the device can actually keep resident.
    return sg::gpu_memory_usage{.budget_bytes = i64(info.Budget), .current_usage_bytes = i64(info.CurrentUsage)};
}
