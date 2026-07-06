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
    if (_queue && _epoch_fence && _fence_event)
        advance_epoch_and_wait_for_idle();

    // Drain + join the download actor and release the ring buffers while the submission fence is still
    // alive (the actor may block on it). The GPU is idle by now, so pending copies complete promptly.
    _download_inline.shutdown();
    _upload_inline.shutdown();
    // The async upload actor runs on the independent copy queue, which advance-and-wait did not drain, so
    // its shutdown waits for that queue to idle. Do it while the copy queue + completion fence are alive.
    _upload_async.shutdown();
    _cmd_pool.shutdown();

    if (_fence_event)
    {
        CloseHandle(_fence_event);
        _fence_event = nullptr;
    }
    _submission_fence.Reset();
    _epoch_fence.Reset();
    _copy_fence.Reset();

    // Release the device-level COM objects (live-object tracking will unwind here later too).
    _copy_queue.Reset();
    _queue.Reset();
    _device.Reset();
    _factory.Reset();
    _is_shut_down = true;
}
} // namespace sg::backend::dx12
