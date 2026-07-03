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

    _allocators.lock(
        [](dx12_allocator_pool& p)
        {
            p.in_epoch = {};
            p.free = {};
        });

    if (_fence_event)
    {
        CloseHandle(_fence_event);
        _fence_event = nullptr;
    }
    _submission_fence.Reset();
    _epoch_fence.Reset();

    // Release the device-level COM objects (live-object tracking will unwind here later too).
    _queue.Reset();
    _device.Reset();
    _factory.Reset();
    _is_shut_down = true;
}
} // namespace sg::backend::dx12
