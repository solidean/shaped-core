// dx12_context: device-level lifetime bodies (shutdown / teardown). The heavier bring-up path
// lives in dx12_context.create.cc.

#include <shaped-graphics/backends/dx12/dx12_context.hh>

namespace sg::backend::dx12
{
void dx12_context::shutdown()
{
    if (_is_shut_down)
        return;

    // Advance-and-wait-for-idle drains the GPU, then closes and retires the final epoch — freeing
    // every resource (in-flight and staged) and running finalizers — before the device is released.
    // Externally synchronized: no create/submit/drop may run concurrently with shutdown.
    if (_queue && _epoch_fence)
        advance_epoch_and_wait_for_idle();

    // Drain + join the download actor and release the ring buffers while the submission fence is still
    // alive (the actor may block on it). The GPU is idle by now, so pending copies complete promptly.
    _download_inline.shutdown();
    _upload_inline.shutdown();
    // The async upload + download actors run on the independent copy queue, which advance-and-wait did not
    // drain, so their shutdown waits for that queue to idle. Do it while the copy queue + fences are alive.
    _upload_async.shutdown();
    _download_async.shutdown();

    // Both the direct and copy queues are idle now. The async actor's own shutdown may have dropped the
    // last reference to a buffer (its in-flight upload) after the final advance already ran, staging a
    // fresh deferred deletion, and copy-deferred hold-backs may still be waiting on the (now fully
    // signaled) copy fence. Nothing else will sweep these, so release them here while the device is alive.
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

    _submission_fence.Reset();
    _epoch_fence.Reset();

    // Release the device-level COM objects (live-object tracking will unwind here later too).
    _queue.Reset();
    _device.Reset();
    _factory.Reset();
    _is_shut_down = true;
}
} // namespace sg::backend::dx12
